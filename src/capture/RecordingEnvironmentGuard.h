#pragma once
// ─────────────────────────────────────────────────────────────────────────────
// RecordingEnvironmentGuard.h — 录屏期间桌面图标与系统任务栏无损隐藏/自动恢复守卫
//
// 安全保证:
//   - 仅隐藏原本处于可见态的桌面 SysListView32 图标层或主副屏任务栏，绝不误显用户原本已隐藏的控件
//   - 支持停止录制、取消倒计时及进程析构三重幂等自动恢复
// ─────────────────────────────────────────────────────────────────────────────

#ifndef TOOLS3000_CAPTURE_RECORDINGENVIRONMENTGUARD_H
#define TOOLS3000_CAPTURE_RECORDINGENVIRONMENTGUARD_H

#include <windows.h>
#include <mutex>
#include <vector>

namespace tools3000::capture {

class RecordingEnvironmentGuard {
public:
    static RecordingEnvironmentGuard& instance();

    /// 在正式进入录制/倒计时前按需净化桌面环境
    void applyForSession(bool hideDesktopIcons, bool hideTaskbar);

    /// 录制期间周期性轻量巡检，防止 Windows 11 Explorer 焦点切换时自动重刷常驻任务栏
    void keepHiddenIfActive() noexcept;

    /// 录制结束、取消或异常退出时幂等恢复全部被临时隐藏的系统窗口与任务栏 AppBar 状态
    void restoreAll() noexcept;

private:
    RecordingEnvironmentGuard() = default;
    ~RecordingEnvironmentGuard() { restoreAll(); }
    RecordingEnvironmentGuard(const RecordingEnvironmentGuard&) = delete;
    RecordingEnvironmentGuard& operator=(const RecordingEnvironmentGuard&) = delete;

    std::mutex m_mutex;
    std::vector<HWND> m_hiddenDesktopIconViews;
    std::vector<HWND> m_hiddenTaskbars;
    bool m_activeHideDesktop = false;
    bool m_activeHideTaskbar = false;
    bool m_modifiedAppBarState = false;
    UINT m_savedAppBarState = 0;
};

} // namespace tools3000::capture

#endif // TOOLS3000_CAPTURE_RECORDINGENVIRONMENTGUARD_H
