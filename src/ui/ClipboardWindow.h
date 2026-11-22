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
    bool isPinned() const noexcept { return m_pinned; }
    bool isMaximized() const noexcept { return m_maximized; }
    void setPinned(bool pinned);
    void toggleMaximize();
    HWND getHwnd() const noexcept { return m_hwnd; }
    HWND previousForegroundHwnd() const noexcept { return m_previousForegroundHwnd; }

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
    void notifyShownToWeb();
    void notifyWindowStateToWeb();

    static LRESULT CALLBACK windowProc(HWND hwnd, UINT uMsg, WPARAM wParam, LPARAM lParam);

    static constexpr UINT_PTR IDT_IDLE_DESTROY = 2102;
    // 隐藏后先经 TrySuspend 瞬降 GPU/JS 活跃占用，5 分钟无交互再彻底销毁 WebView2 归还全部物理内存，
    // 兼顾高频连续呼出 0ms 秒开体验与长时闲置零残留。
    static constexpr UINT IDLE_DESTROY_TIMEOUT_MS = 300000;
    static constexpr UINT WM_CLIPBOARD_VERIFY_DEACTIVATED = WM_APP + 29;

    HWND m_hwnd = nullptr;
    HWND m_previousForegroundHwnd = nullptr;
    Microsoft::WRL::ComPtr<ICoreWebView2Environment> m_environment;
    Microsoft::WRL::ComPtr<ICoreWebView2Controller> m_controller;
    Microsoft::WRL::ComPtr<ICoreWebView2> m_webView;
    WebViewSuspendController m_suspendController;

    std::atomic<bool> m_visible{false};
    std::atomic<uint64_t> m_generation{0};
    bool m_webViewReady = false;
    bool m_pinned = false;
    bool m_maximized = false;
    bool m_inSizeMove = false;
    bool m_hasCustomBounds = false;
    RECT m_customBounds{};
    uint64_t m_showTimeTick = 0;
};

}  // namespace tools3000::ui

#endif  // TOOLS3000_UI_CLIPBOARDWINDOW_H
