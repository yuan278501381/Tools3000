#pragma once
// ─────────────────────────────────────────────────────────────────────────────
// TrayIcon — 系统托盘图标管理器
//
// 特性:
//   1. 托盘图标 + 气泡通知
//   2. 右键菜单（暂停手势/截图/录屏/设置/退出）
//   3. 双击打开设置窗口
//   4. 状态变化时切换图标（正常/暂停/录制中）
// ─────────────────────────────────────────────────────────────────────────────

#ifndef TOOLS3000_TRAY_TRAYICON_H
#define TOOLS3000_TRAY_TRAYICON_H

#include <windows.h>
#include <shellapi.h>
#include <cstdint>
#include <string>
#include <functional>
#include <unordered_map>

#ifndef NIN_SELECT
#define NIN_SELECT (WM_USER + 0)
#endif
#ifndef NIN_KEYSELECT
#define NIN_KEYSELECT (NIN_SELECT | 1)
#endif

namespace tools3000::tray {

/// 托盘菜单项 ID
enum class TrayMenuId : UINT {
    OpenSettings    = 1001,
    Screenshot      = 1002,
    Recording       = 1003,
    PauseGesture    = 1004,
    Search          = 1005,
    RestartElevated = 1006,
    Separator       = 0,
    Exit            = 1099
};

/// 托盘事件回调类型
using TrayEventCallback = std::function<void()>;

class TrayIcon {
public:
    static TrayIcon& instance();

    /// 创建托盘图标
    /// @param hwnd 消息窗口句柄
    /// @param icon 图标句柄（可选）
    bool create(HWND hwnd, HICON icon = nullptr);

    /// 确保托盘图标已成功创建，若尚未成功则尝试创建并返回状态
    bool ensureCreated(HWND hwnd = nullptr);

    /// 重建托盘图标（例如 Explorer 重启或被二次唤醒时）
    void recreate();

    /// 查询托盘图标是否已成功挂载
    bool isCreated() const { return m_created; }

    /// 销毁托盘图标
    void destroy();

    /// 显示气泡通知
    void showNotification(const std::wstring& title, const std::wstring& message,
                          DWORD iconType = NIIF_INFO, UINT timeoutMs = 3000);

    /// 切换图标（用于状态变化：正常/暂停/录制中）
    void setIcon(HICON icon);

    /// 设置提示文本
    void setTooltip(const std::wstring& tooltip);

    /// 同步手势暂停状态（用于菜单勾选和提示文字）
    void setGesturePaused(bool paused);
    bool isGesturePaused() const { return m_gesturePaused; }

    /// 确保已订阅手势暂停事件总线（幂等）
    void ensureEventSubscriptions();

    /// 获取当前提示文本（用于状态断言）
    std::wstring tooltip() const { return m_nid.szTip; }

    /// 根据当前 Windows 任务栏深浅主题热刷新托盘图标
    void refreshThemeIcon();

    /// 处理托盘消息（在窗口过程中调用）
    void handleMessage(WPARAM wParam, LPARAM lParam);

    // ── 事件绑定 ─────────────────────────────────────────────────────────

    void onOpenSettings(TrayEventCallback callback) { m_callbacks[TrayMenuId::OpenSettings] = std::move(callback); }
    void onScreenshot(TrayEventCallback callback)    { m_callbacks[TrayMenuId::Screenshot]   = std::move(callback); }
    void onRecording(TrayEventCallback callback)     { m_callbacks[TrayMenuId::Recording]    = std::move(callback); }
    void onSearch(TrayEventCallback callback)        { m_callbacks[TrayMenuId::Search]       = std::move(callback); }
    void onPauseGesture(TrayEventCallback callback)  { m_callbacks[TrayMenuId::PauseGesture] = std::move(callback); }
    void onRestartElevated(TrayEventCallback callback) { m_callbacks[TrayMenuId::RestartElevated] = std::move(callback); }
    void onExit(TrayEventCallback callback)          { m_callbacks[TrayMenuId::Exit]         = std::move(callback); }

    /// 显示右键菜单（支持传入指定坐标，默认 -1 自适应获取光标或托盘位置）
    void showContextMenu(int x = -1, int y = -1);

    /// 显示 Windows 原生右键菜单 (零延迟/高可靠兜底)
    void showNativeContextMenu(POINT pt);

    /// 触发菜单回调
    void fireCallback(TrayMenuId id);

    /// 检查当前鼠标物理光标是否正悬浮在 Tools3000 托盘图标几何矩形区域内
    bool isCursorOnTrayIcon() const;

    static constexpr UINT TIMER_ID_TRAY_RETRY = 2001;
    static constexpr UINT WM_TRAYICON = WM_USER + 100;

private:
    TrayIcon();
    TrayIcon(const TrayIcon&) = delete;
    TrayIcon& operator=(const TrayIcon&) = delete;

    NOTIFYICONDATAW m_nid{};
    HWND m_hwnd = nullptr;
    HICON m_icon = nullptr;
    UINT m_currentIconId = 0;
    uint64_t m_lastThemeRefreshTick = 0;
    uint64_t m_gesturePauseSubId = 0;
    bool m_created = false;
    bool m_gesturePaused = false;
    UINT m_lastRightClickMsg = 0;
    uint64_t m_lastRightClickTick = 0;
    std::unordered_map<TrayMenuId, TrayEventCallback> m_callbacks;
};

}  // namespace tools3000::tray

#endif  // TOOLS3000_TRAY_TRAYICON_H
