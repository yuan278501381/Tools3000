#include "capture/RecordingEnvironmentGuard.h"

#include <algorithm>

namespace tools3000::capture {

namespace {

void collectDesktopListViews(std::vector<HWND>& outViews) {
    auto checkHost = [&](HWND host) {
        if (!host) return;
        HWND defView = FindWindowExW(host, nullptr, L"SHELLDLL_DefView", nullptr);
        if (!defView) return;
        HWND listView = FindWindowExW(defView, nullptr, L"SysListView32", nullptr);
        if (listView && IsWindow(listView)) {
            if (std::find(outViews.begin(), outViews.end(), listView) == outViews.end()) {
                outViews.push_back(listView);
            }
        }
    };

    checkHost(FindWindowW(L"Progman", nullptr));
    EnumWindows([](HWND hwnd, LPARAM lParam) -> BOOL {
        wchar_t cls[64]{};
        if (GetClassNameW(hwnd, cls, 64) > 0 && wcscmp(cls, L"WorkerW") == 0) {
            HWND defView = FindWindowExW(hwnd, nullptr, L"SHELLDLL_DefView", nullptr);
            if (defView) {
                HWND lv = FindWindowExW(defView, nullptr, L"SysListView32", nullptr);
                if (lv && IsWindow(lv)) {
                    auto* vec = reinterpret_cast<std::vector<HWND>*>(lParam);
                    if (std::find(vec->begin(), vec->end(), lv) == vec->end()) {
                        vec->push_back(lv);
                    }
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
    std::lock_guard lock(m_mutex);
    // 先清理可能残留的上一轮状态
    for (HWND h : m_hiddenDesktopIconViews) {
        if (h && IsWindow(h)) ShowWindowAsync(h, SW_SHOWNA);
    }
    m_hiddenDesktopIconViews.clear();
    for (HWND h : m_hiddenTaskbars) {
        if (h && IsWindow(h)) ShowWindowAsync(h, SW_SHOWNA);
    }
    m_hiddenTaskbars.clear();

    if (hideDesktopIcons) {
        std::vector<HWND> candidates;
        collectDesktopListViews(candidates);
        for (HWND h : candidates) {
            if (IsWindowVisible(h)) {
                ShowWindowAsync(h, SW_HIDE);
                m_hiddenDesktopIconViews.push_back(h);
            }
        }
    }

    if (hideTaskbar) {
        std::vector<HWND> bars;
        collectTaskbarWindows(bars);
        for (HWND h : bars) {
            if (IsWindowVisible(h)) {
                ShowWindowAsync(h, SW_HIDE);
                m_hiddenTaskbars.push_back(h);
            }
        }
    }
}

void RecordingEnvironmentGuard::restoreAll() noexcept {
    std::lock_guard lock(m_mutex);
    for (HWND h : m_hiddenDesktopIconViews) {
        if (h && IsWindow(h)) {
            ShowWindow(h, SW_SHOWNA);
        }
    }
    m_hiddenDesktopIconViews.clear();

    for (HWND h : m_hiddenTaskbars) {
        if (h && IsWindow(h)) {
            ShowWindow(h, SW_SHOWNA);
        }
    }
    m_hiddenTaskbars.clear();
}

} // namespace tools3000::capture
