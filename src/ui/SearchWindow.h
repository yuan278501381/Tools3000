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
    void minimize();
    bool isVisible() const;
    bool isMinimized() const;
    bool isForegroundActive() const;
    void destroy();
    void setWindowSize(int baseWidth, int baseHeight, bool forceCenter = false);
    std::pair<int, int> getWindowSize() const;
    void setMaximized(bool maximized);
    void toggleMaximized();
    bool isMaximized() const { return m_isMaximized.load(); }
    void setPinned(bool pinned);
    bool isPinned() const { return m_isPinned.load(); }
    void dragMove();
    void showSystemMenu(int screenX = -1, int screenY = -1);
    HWND getHwnd() const { return m_hwnd; }
    void focusSearchIfVisible();
    void markFirstPaintReady();
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

    /// 原生独立应用窗口样式单一事实源：具备任务栏图标、Alt+Tab、最小化/最大化、Aero Snap 与系统菜单
    static constexpr DWORD NativeWindowExStyle = WS_EX_APPWINDOW;
    static constexpr DWORD NativeWindowStyle = WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN;

    enum class ToggleAction {
        ShowOrFocus,
        Minimize,
    };

    /// Alt+Space / 托盘呼出行为决策：仅当前台激活可见时最小化到任务栏，其余状态一律拉起聚焦
    static constexpr ToggleAction resolveToggleAction(
        bool visible, bool minimized, bool foreground) noexcept {
        return (visible && !minimized && foreground)
            ? ToggleAction::Minimize
            : ToggleAction::ShowOrFocus;
    }

    struct BlurHideContext {
        bool hideOnBlurEnabled = false;
        bool visible = false;
        bool minimized = false;
        bool foregroundActive = false;
        bool pinned = false;
        bool menuOrDragActive = false;
        bool quickLookVisible = false;
        bool foregroundOwnedBySearchOrMenu = false;
        uint64_t elapsedSinceShowMs = 0;
    };

    static constexpr uint64_t BlurHideShowGraceMs = 250;

    /// 失焦自动隐藏纯函数决策（严防置顶、QuickLook 预览、右键/系统菜单、子对话框与刚呼出焦点抖动误关）
    static constexpr bool shouldAutoHideOnBlur(const BlurHideContext& ctx) noexcept {
        if (!ctx.hideOnBlurEnabled) return false;
        if (!ctx.visible || ctx.minimized || ctx.foregroundActive) return false;
        if (ctx.pinned || ctx.menuOrDragActive || ctx.quickLookVisible) return false;
        if (ctx.foregroundOwnedBySearchOrMenu) return false;
        if (ctx.elapsedSinceShowMs < BlurHideShowGraceMs) return false;
        return true;
    }

    /// 呼出搜索框时是否需要提前唤醒后台全盘索引服务（默认分类为纯离线 app / bookmark 时不唤醒，省去 100% 服务开销）
    static constexpr bool shouldWarmUpSearchServiceOnShow(std::string_view defaultCategory) noexcept {
        return defaultCategory != "app" && defaultCategory != "bookmark";
    }

    /// 首次显露或挂起/最小化唤醒时启用原生 DWMWA_CLOAK 隐身热身，等待 WebView2 提交新帧后原子揭幕（杜绝 Win10/核显短暂黑屏闪烁）
    static constexpr bool shouldCloakOnShow(
        bool webViewReady,
        bool uiFirstPaintReady,
        bool wasSuspended,
        bool wasMinimized) noexcept {
        return webViewReady && (!uiFirstPaintReady || wasSuspended || wasMinimized);
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
    void persistNormalGeometry();

    static LRESULT CALLBACK windowProc(HWND hwnd, UINT uMsg, WPARAM wParam, LPARAM lParam);

    static constexpr UINT_PTR IDT_DEFERRED_SUSPEND = 2110;
    static constexpr UINT DEFERRED_SUSPEND_DELAY_MS = 25000;
    static constexpr UINT_PTR IDT_COLD_WARMUP_FALLBACK = 2111;
    static constexpr UINT COLD_WARMUP_FALLBACK_DELAY_MS = 1500;
    static constexpr UINT_PTR IDT_FIRST_PAINT_FALLBACK = 2112;
    static constexpr UINT FIRST_PAINT_FALLBACK_DELAY_MS = 2000;
    static constexpr UINT_PTR IDT_DEFERRED_BLUR_HIDE = 2113;
    static constexpr UINT DEFERRED_BLUR_HIDE_DELAY_MS = 60;
    static constexpr UINT_PTR IDT_POST_SHOW_REPAINT_VERIFY = 2114;
    static constexpr UINT POST_SHOW_REPAINT_VERIFY_MS = 160;
    static constexpr UINT_PTR IDT_FIRST_PAINT_UNCLOAK = 2115;
    static constexpr UINT FIRST_PAINT_UNCLOAK_TIMEOUT_MS = 450;

    enum class WebViewStatus : uint8_t {
        Loading = 0,
        Ready = 1,
        MissingRuntime = 2,
        InitFailed = 3,
        RenderCrashed = 4,
    };

    void setWebViewStatus(WebViewStatus status, HRESULT hr = S_OK);
    bool setWindowCloaked(bool cloaked);
    void completeFirstPaintReady();
    void presentWebViewSurface(bool forceCompositorKick);
    void paintStatusBackdrop(HDC hdc, const RECT& clientRect) const;
    void handleStatusSurfaceAction();

    HWND m_hwnd = nullptr;
    Microsoft::WRL::ComPtr<ICoreWebView2Environment> m_environment;
    Microsoft::WRL::ComPtr<ICoreWebView2Controller> m_controller;
    Microsoft::WRL::ComPtr<ICoreWebView2> m_webView;
    WebViewSuspendController m_suspendController;

    std::atomic<bool> m_visible{false};
    std::atomic<bool> m_minimized{false};
    std::atomic<bool> m_webViewReady{false};
    std::atomic<bool> m_uiFirstPaintReady{false};
    std::atomic<bool> m_webViewInitializing{false};
    std::atomic<bool> m_pendingServiceWarmup{false};
    std::atomic<uint8_t> m_webViewStatus{static_cast<uint8_t>(WebViewStatus::Loading)};
    std::atomic<uint32_t> m_lastWebViewErrorHr{0};
    std::atomic<bool> m_autoRecoverAttempted{false};
    std::atomic<bool> m_isPinned{false};
    std::atomic<bool> m_isMaximized{false};
    std::atomic<bool> m_menuActive{false};
    std::atomic<bool> m_inSizeMove{false};
    std::atomic<uint64_t> m_lastMenuCloseTick{0};
    bool m_updatingPlacement = false;
    bool m_wasSuspended = false;
    bool m_everPresentedVisible = false;
    bool m_cloakedForFirstPaint = false;
    uint64_t m_showTimeTick{0};
    std::atomic<uint64_t> m_generation{0};
};

} // namespace tools3000::ui
