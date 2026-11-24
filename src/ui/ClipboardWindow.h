#pragma once
// ─────────────────────────────────────────────────────────────────────────────
// ClipboardWindow.h — 极速呼出剪贴板历史无边框悬浮面板 (WebView2 独立轻量表面)
//
// 职责:
//   1. 全局热键 (默认 Alt+V) 毫秒级呼出于鼠标所在显示器，自动记忆先前前台窗口；
//   2. 映射 %LOCALAPPDATA%\Tools3000\clipboard\blobs 到 clipboard-blobs.tools3000.local，
//      图片历史缩略图零 Base64 内存开销流式渲染；
//   3. 严格接入 applyUniversalRoundedCorners 双保险、KeyboardPipeline 与 60s 闲置自动销毁。
// ─────────────────────────────────────────────────────────────────────────────

#ifndef TOOLS3000_UI_CLIPBOARDWINDOW_H
#define TOOLS3000_UI_CLIPBOARDWINDOW_H

#include <windows.h>
#include <wrl/client.h>
#include <atomic>
#include <cstdint>
#include <string>
#include "ui/WebViewSuspend.h"

struct ICoreWebView2;
struct ICoreWebView2Controller;
struct ICoreWebView2Environment;

namespace tools3000::ui {

class ClipboardWindow final {
public:
    static ClipboardWindow& instance();

    void toggle(HINSTANCE hInstance = nullptr);
    void show(HINSTANCE hInstance = nullptr);
    void hide();
    void destroy();

    bool isVisible() const noexcept;
    bool isForegroundActive() const noexcept;
    bool isPinned() const noexcept { return m_pinned; }
    bool isMaximized() const noexcept { return m_maximized; }
    void setPinned(bool pinned);
    void toggleMaximize();
    HWND getHwnd() const noexcept { return m_hwnd; }
    HWND previousForegroundHwnd() const noexcept { return m_previousForegroundHwnd; }

    /// 过滤空句柄、自身及其子窗口与不可见/任务栏窗口，归一化为顶层 GA_ROOT 句柄
    static HWND resolveValidTargetForegroundHwnd(HWND candidate, HWND selfHwnd) noexcept;

    /// 判定按下呼出热键时应隐藏还是强力前置显示（冷启等待中或掉到后台被遮挡时一律前置显示，绝不误关）
    static constexpr bool shouldHideOnToggle(
        bool visible,
        bool pendingReveal,
        bool foregroundActive) noexcept {
        return visible && !pendingReveal && foregroundActive;
    }

    /// 判定失焦时是否允许自动隐藏剪贴板窗口（置顶、缩放、原生拖拽、拖拽会话驻留期及拖拽结束后 1200ms 内严禁自动隐藏）
    static bool shouldAutoHideOnDeactivate(
        bool visible,
        bool pendingReveal,
        bool pinned,
        bool inSizeMove,
        bool inNativeDrag,
        uint64_t nowTick,
        uint64_t showTick,
        uint64_t lastNativeDragEndTick,
        bool dragSessionHold = false) noexcept {
        if (!visible || pendingReveal || pinned || inSizeMove || inNativeDrag || dragSessionHold) {
            return false;
        }
        if (nowTick >= showTick && (nowTick - showTick) <= 200) {
            return false;
        }
        if (lastNativeDragEndTick != 0 &&
            nowTick >= lastNativeDragEndTick &&
            (nowTick - lastNativeDragEndTick) < NATIVE_DRAG_HIDE_GRACE_MS) {
            return false;
        }
        return true;
    }

    /// 向已创建的剪贴板 WebView 推送实时更新或主题事件
    void pushEventJson(const std::string& envelopeJson);
    void applyThemeChrome();

private:
    ClipboardWindow() = default;
    ~ClipboardWindow();
    ClipboardWindow(const ClipboardWindow&) = delete;
    ClipboardWindow& operator=(const ClipboardWindow&) = delete;

    bool createWindow(HINSTANCE hInstance);
    void initializeWebView2();
    void updatePlacement();
    void revealWindowNow();
    void enterDeferredSuspend();
    void notifyShownToWeb();
    void notifyWindowStateToWeb();
    void installForegroundTracker();
    void uninstallForegroundTracker();

    static LRESULT CALLBACK windowProc(HWND hwnd, UINT uMsg, WPARAM wParam, LPARAM lParam);
    static void CALLBACK foregroundWinEventProc(
        HWINEVENTHOOK hWinEventHook,
        DWORD event,
        HWND hwnd,
        LONG idObject,
        LONG idChild,
        DWORD idEventThread,
        DWORD dwmsEventTime);

    static constexpr UINT_PTR IDT_IDLE_DESTROY = 2102;
    static constexpr UINT_PTR IDT_DEFERRED_SUSPEND = 2103;
    static constexpr UINT_PTR IDT_REVEAL_FALLBACK = 2104;
    // 高频热保活宽限期 (25s)：隐藏后 25 秒内保留 GPU 合成帧不立刻 TrySuspend/trimWorkingSet，
    // 连续呼出 <16ms 零黑屏瞬开；超 25s 软挂起降内存，超 5 分钟彻底销毁归还全部物理内存。
    static constexpr UINT DEFERRED_SUSPEND_DELAY_MS = 25000;
    static constexpr UINT IDLE_DESTROY_TIMEOUT_MS = 90000;
    static constexpr UINT REVEAL_FALLBACK_WARM_MS = 450;
    static constexpr UINT REVEAL_FALLBACK_COLD_MS = 1200;
    static constexpr uint64_t NATIVE_DRAG_HIDE_GRACE_MS = 1200;
    static constexpr UINT WM_CLIPBOARD_VERIFY_DEACTIVATED = WM_APP + 29;
    static constexpr UINT WM_CLIPBOARD_CAPTURE_FOREGROUND = WM_APP + 30;

    HWND m_hwnd = nullptr;
    HWND m_previousForegroundHwnd = nullptr;
    HWINEVENTHOOK m_fgEventHook = nullptr;
    Microsoft::WRL::ComPtr<ICoreWebView2Environment> m_environment;
    Microsoft::WRL::ComPtr<ICoreWebView2Controller> m_controller;
    Microsoft::WRL::ComPtr<ICoreWebView2> m_webView;
    WebViewSuspendController m_suspendController;

    std::atomic<bool> m_visible{false};
    std::atomic<uint64_t> m_generation{0};
    std::atomic<bool> m_inNativeDrag{false};
    bool m_webViewReady = false;
    bool m_suspendedOrCold = true;
    bool m_pendingReveal = false;
    bool m_pinned = false;
    bool m_dragSessionHold = false;
    bool m_maximized = false;
    bool m_inSizeMove = false;
    bool m_hasCustomBounds = false;
    RECT m_customBounds{};
    uint64_t m_showTimeTick = 0;
    uint64_t m_lastNativeDragEndTick = 0;
};

}  // namespace tools3000::ui

#endif  // TOOLS3000_UI_CLIPBOARDWINDOW_H
