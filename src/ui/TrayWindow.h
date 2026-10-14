#pragma once
// ─────────────────────────────────────────────────────────────────────────────
// TrayWindow.h — WebView2 托盘无边框悬浮菜单
// ─────────────────────────────────────────────────────────────────────────────

#ifndef TOOLS3000_UI_TRAYWINDOW_H
#define TOOLS3000_UI_TRAYWINDOW_H

#include <windows.h>
#include <atomic>
#include <cstdint>
#include <wrl/client.h>
#include <WebView2.h>
#include "ui/WebViewSuspend.h"

namespace tools3000::ui {

class TrayWindow {
public:
    static TrayWindow& instance();

    /// 预热托盘菜单 WebView2 渲染环境（后台静默就绪，使用户右键时 0 毫秒瞬间呼出）
    void preload(HINSTANCE hInstance);

    /// 显示托盘菜单，并将其定位到指定的坐标附近 (通常是鼠标点击系统托盘的位置)
    void show(HINSTANCE hInstance, int x, int y);

    /// 动态设置托盘菜单内容实际尺寸
    void setContentSize(int width, int height);

    /// 隐藏
    void hide();

    /// 是否可见
    bool isVisible() const;

    /// 销毁
    void destroy();

    /// 查询 WebView2 渲染环境是否已就绪
    bool isWebViewReady() const { return m_webViewReady.load(); }

    /// 设置 WebView2 就绪状态（供生命周期及测试桩注入使用）
    void setWebViewReady(bool ready) { m_webViewReady.store(ready); }

    /// 获取最后一次显示的时间戳 (GetTickCount64)
    uint64_t getShowTimeTick() const { return m_showTimeTick; }

    /// 获取最后一次隐藏的时间戳 (GetTickCount64)，用于防抖防闪烁时序计算
    uint64_t lastHideTimeTick() const { return m_lastHideTimeTick.load(); }

    /// 获取唤起锚点物理坐标
    POINT anchor() const { return m_anchor; }

    /// 判断指定窗口是否属于任务栏或托盘溢出窗口白名单 (Shell_TrayWnd, Shell_SecondaryTrayWnd, NotifyIconOverflowWindow, TopLevelWindowForOverflowXamlIsland, XamlExplorerHostIslandWindow, TrayNotifyWnd, SysPager, ToolbarWindow32)
    static inline bool isTaskbarOrOverflowWindow(HWND hwnd) {
        if (!hwnd || !IsWindow(hwnd)) return false;
        auto matchClass = [](HWND h) {
            wchar_t cls[128] = {};
            if (GetClassNameW(h, cls, static_cast<int>(sizeof(cls) / sizeof(cls[0]))) == 0) return false;
            if (_wcsicmp(cls, L"Shell_TrayWnd") == 0 ||
                _wcsicmp(cls, L"Shell_SecondaryTrayWnd") == 0 ||
                _wcsicmp(cls, L"NotifyIconOverflowWindow") == 0 ||
                _wcsicmp(cls, L"TopLevelWindowForOverflowXamlIsland") == 0 ||
                _wcsicmp(cls, L"XamlExplorerHostIslandWindow") == 0 ||
                _wcsicmp(cls, L"TrayNotifyWnd") == 0 ||
                _wcsicmp(cls, L"SysPager") == 0 ||
                _wcsicmp(cls, L"ToolbarWindow32") == 0) {
                return true;
            }
            // 对 Windows 11 WinUI/XAML 岛屿窗口增强检测 (DesktopWindowContentBridge 归属于 Shell 进程时属于任务栏/托盘)
            if (_wcsicmp(cls, L"Windows.UI.Composition.DesktopWindowContentBridge") == 0) {
                HWND shell = GetShellWindow();
                if (shell) {
                    DWORD shellPid = 0, targetPid = 0;
                    GetWindowThreadProcessId(shell, &shellPid);
                    GetWindowThreadProcessId(h, &targetPid);
                    if (shellPid != 0 && targetPid == shellPid) return true;
                }
            }
            return false;
        };
        if (matchClass(hwnd)) return true;
        HWND root = GetAncestor(hwnd, GA_ROOT);
        if (root && root != hwnd && matchClass(root)) return true;
        return false;
    }

private:
    TrayWindow() = default;
    ~TrayWindow() { destroy(); }
    TrayWindow(const TrayWindow&) = delete;
    TrayWindow& operator=(const TrayWindow&) = delete;

    bool createWindow(HINSTANCE hInstance, int x, int y);
    void initializeWebView2();
    void updatePlacement();
    static LRESULT CALLBACK windowProc(HWND hwnd, UINT uMsg, WPARAM wParam, LPARAM lParam);

private:
    HWND m_hwnd = nullptr;
    std::atomic<bool> m_visible{false};
    std::atomic<bool> m_webViewReady{false};
    std::atomic<uint64_t> m_generation{0};
    uint64_t m_showTimeTick{0};
    std::atomic<uint64_t> m_lastHideTimeTick{0};
    POINT m_anchor{};
    bool m_updatingPlacement = false;
    int m_contentWidth = 0;
    int m_contentHeight = 0;

    Microsoft::WRL::ComPtr<ICoreWebView2Environment> m_environment;
    Microsoft::WRL::ComPtr<ICoreWebView2Controller> m_controller;
    Microsoft::WRL::ComPtr<ICoreWebView2> m_webView;
    WebViewSuspendController m_suspendController;
};

} // namespace tools3000::ui

#endif // TOOLS3000_UI_TRAYWINDOW_H
