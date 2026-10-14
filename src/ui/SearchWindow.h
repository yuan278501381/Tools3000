#pragma once
#include "ui/WebViewSuspend.h"
#include "core/utils/DpiUtils.h"
#include <windows.h>
#include <wrl/client.h>
#include <string>
#include <atomic>
#include <cstdint>

struct ICoreWebView2;
struct ICoreWebView2Controller;
struct ICoreWebView2Environment;

namespace tools3000::ui {

class SearchWindow {
public:
    static SearchWindow& instance();

    /// 预热搜索窗口 WebView2 渲染环境（后台静默就绪，使用户按下快捷键时 0 毫秒瞬间呼出）
    void preload(HINSTANCE hInstance);

    void show(HINSTANCE hInstance);
    void hide();
    bool isVisible() const;
    void destroy();
    void setWindowSize(int baseWidth, int baseHeight, bool forceCenter = false);
    std::pair<int, int> getWindowSize() const;
    void setPinned(bool pinned);
    bool isPinned() const { return m_isPinned.load(); }
    HWND getHwnd() const { return m_hwnd; }
    void focusSearchIfVisible();
    void triggerQuickLookForSelection();
    void syncQuickLookForSelection();
    void stepQuickLookSelection(int delta);
    void setMenuActive(bool active) {
        m_menuActive.store(active);
        if (!active) {
            m_lastMenuCloseTick.store(GetTickCount64());
        }
    }
    bool isMenuActive() const {
        if (m_inSizeMove.load()) return true;
        if (m_menuActive.load()) return true;
        if (m_hwnd && GetPropW(m_hwnd, L"Tools3000_ShellMenuActive")) return true;
        const uint64_t now = GetTickCount64();
        const uint64_t closeTick = m_lastMenuCloseTick.load();
        if (closeTick > 0 && now >= closeTick && (now - closeTick) < 600) {
            return true;
        }
        return false;
    }

    /// 计算窗口非客户区命中测试结果（支持按 DPI 比例校准，全高放行边缘拉伸）
    static LRESULT calculateHitTest(const RECT& rc, POINT pt, float scale) {
        const int corner = tools3000::core::dpi::scaleMetric(8, scale);
        const int border = tools3000::core::dpi::scaleMetric(6, scale);

        if (pt.y < rc.top + corner && pt.x < rc.left + corner) return HTTOPLEFT;
        if (pt.y < rc.top + corner && pt.x >= rc.right - corner) return HTTOPRIGHT;
        if (pt.y >= rc.bottom - corner && pt.x < rc.left + corner) return HTBOTTOMLEFT;
        if (pt.y >= rc.bottom - corner && pt.x >= rc.right - corner) return HTBOTTOMRIGHT;
        if (pt.y < rc.top + border) return HTTOP;
        if (pt.y >= rc.bottom - border) return HTBOTTOM;
        if (pt.x < rc.left + border) return HTLEFT;
        if (pt.x >= rc.right - border) return HTRIGHT;

        return HTCLIENT;
    }

private:
    SearchWindow() = default;
    ~SearchWindow() { destroy(); }
    SearchWindow(const SearchWindow&) = delete;
    SearchWindow& operator=(const SearchWindow&) = delete;

    bool createWindow(HINSTANCE hInstance);
    void initializeWebView2();
    void updatePlacement();

    static LRESULT CALLBACK windowProc(HWND hwnd, UINT uMsg, WPARAM wParam, LPARAM lParam);

    HWND m_hwnd = nullptr;
    Microsoft::WRL::ComPtr<ICoreWebView2Environment> m_environment;
    Microsoft::WRL::ComPtr<ICoreWebView2Controller> m_controller;
    Microsoft::WRL::ComPtr<ICoreWebView2> m_webView;
    WebViewSuspendController m_suspendController;

    std::atomic<bool> m_visible{false};
    std::atomic<bool> m_webViewReady{false};
    std::atomic<bool> m_isPinned{false};
    std::atomic<bool> m_menuActive{false};
    std::atomic<bool> m_inSizeMove{false};
    std::atomic<uint64_t> m_lastMenuCloseTick{0};
    bool m_updatingPlacement = false;
    uint64_t m_showTimeTick{0};
    std::atomic<uint64_t> m_generation{0};
};

} // namespace tools3000::ui
