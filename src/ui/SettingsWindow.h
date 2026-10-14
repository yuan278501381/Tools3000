#pragma once
// ─────────────────────────────────────────────────────────────────────────────
// SettingsWindow — WebView2 设置窗口
//
// 职责:
//   1. 创建 Win32 顶层窗口承载 WebView2 控件
//   2. 初始化 WebView2 环境（Evergreen Runtime）
//   3. 加载前端 UI（本地文件或开发服务器）
//   4. 桥接 JS ↔ C++ 双向通信（通过 MessageBridge）
//   5. 窗口生命周期管理（显示/隐藏/最小化到托盘）
// ─────────────────────────────────────────────────────────────────────────────

#ifndef TOOLS3000_UI_SETTINGSWINDOW_H
#define TOOLS3000_UI_SETTINGSWINDOW_H

#include "ui/WebViewWindowStyle.h"
#include "ui/WebViewSuspend.h"

#include <windows.h>
#include <string>
#include <functional>
#include <atomic>
#include <chrono>
#include <memory>
#include <cstdint>

// WebView2 头文件（来自 NuGet 包）
#include <wrl/client.h>

// 前向声明 WebView2 接口（避免在头文件中引入完整 WebView2 SDK）
struct ICoreWebView2;
struct ICoreWebView2Controller;
struct ICoreWebView2Environment;

namespace tools3000::ui {

/// 设置窗口配置
struct SettingsWindowConfig {
    int width  = SettingsWindowStyle::BaseWidth;
    int height = SettingsWindowStyle::BaseHeight;
    int posX = 0;
    int posY = 0;
    bool hasCustomPlacement = false; // 命令行或已持久化的物理像素位置（可为负，副屏）
    bool startCentered = true;      // 是否居中显示
    bool devToolsEnabled = true;    // 是否启用开发者工具（仅 Debug）
    std::string devServerUrl;       // 开发服务器 URL（空则使用本地文件）
};

class SettingsWindow {
public:
    static SettingsWindow& instance();

    /// 获取底层 Win32 HWND 句柄
    HWND hwnd() const { return m_hwnd; }

    /// 创建并显示设置窗口
    void show(HINSTANCE hInstance);

    /// 后台静默创建并预热 WebView2 环境，不显示窗口（实现极速冷启动）
    void preload(HINSTANCE hInstance);

    /// 隐藏窗口（不销毁，下次 show 时直接显示）
    void hide();

    /// 窗口是否可见
    bool isVisible() const;

    /// 销毁窗口
    void destroy();

    /// 设置配置
    void setConfig(const SettingsWindowConfig& config) { m_config = config; }

    /// 导航到指定页面（如 "#/gesture" 直接跳转到手势设置页）
    void navigateTo(const std::string& path);

    /// 向前端推送事件
    void pushEventToFrontend(const std::string& eventName, const std::string& dataJson);

    /// 一体化沉浸式标题栏与边缘窗口控制
    void minimize();
    void toggleMaximize();
    void close();
    void dragMove();
    void showSystemMenu(int screenX = -1, int screenY = -1);
    void startResize(const std::string& edge);
    bool isMaximized() const;

private:
    SettingsWindow() = default;
    SettingsWindow(const SettingsWindow&) = delete;
    SettingsWindow& operator=(const SettingsWindow&) = delete;

    /// 创建 Win32 窗口
    bool createWindow(HINSTANCE hInstance);

    /// 初始化 WebView2 环境
    void initializeWebView2();

    /// WebView2 初始化完成后的回调
    void onWebView2Ready();

    /// 获取前端 UI 入口 URL
    std::string getUIEntryUrl() const;

    /// 从配置恢复上次的窗口位置/尺寸（按保存时 DPI 缩放到当前显示器）
    void applyPersistedPlacementIfAny();

    /// 将当前窗口矩形持久化（物理像素 + DPI）
    void persistGeometry();

    /// 窗口过程
    static LRESULT CALLBACK windowProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);

    void smoothPresent();

    // 闲置自动销毁定时器 (用户关闭 1 分钟后彻底销毁 WebView2 渲染器并释放物理内存)
    static constexpr UINT_PTR IDT_IDLE_DESTROY = 1001;
    static constexpr UINT IDLE_DESTROY_TIMEOUT_MS = 60000;

    HWND m_hwnd = nullptr;
    HINSTANCE m_hInstance = nullptr;
    SettingsWindowConfig m_config;
    std::atomic<bool> m_visible{false};
    bool m_webViewReady = false;
    bool m_showWhenReady = false;
    std::atomic<uint64_t> m_generation{0};
    std::chrono::steady_clock::time_point m_initializationStartedAt{};
    std::chrono::steady_clock::time_point m_showRequestedAt{};

    // WebView2 组件（使用 void* 避免头文件依赖）
    Microsoft::WRL::ComPtr<ICoreWebView2Controller> m_controller;
    Microsoft::WRL::ComPtr<ICoreWebView2> m_webView;
    Microsoft::WRL::ComPtr<ICoreWebView2Environment> m_environment;
    WebViewSuspendController m_suspendController;
};

}  // namespace tools3000::ui

#endif  // TOOLS3000_UI_SETTINGSWINDOW_H
