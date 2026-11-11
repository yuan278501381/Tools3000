#pragma once
// ─────────────────────────────────────────────────────────────────────────────
// RemoteMasterEngine.h — 远程协助主控增强引擎 (Remote Desktop Host Boost)
//
// 核心落地的三大功能引擎:
//   1. 沉浸式系统热键直通 (Immersive Remote Hotkey Tunnel):
//      前台聚焦于远控客户端时，智能拦截本地 Windows 对 Win、Win+R、Win+E、
//      Win+D、Win+X、Alt+Tab、Ctrl+Shift+Esc 等系统级保留热键的优先消费，
//      通过硬件扫描码精准投递给远控窗口画布；失焦毫秒级自动解挂。
//   2. 远程修饰键卡死一键急救 (Modifier Key Emergency Flush):
//      当远端发生丢包导致修饰键卡死粘滞时，通过原子化 SendInput 连发全量 8 组
//      修饰键与鼠标按键的 KEYUP 脉冲，瞬间解除粘滞状态。
//   3. 远控输入法智能脱敏 (Smart Remote IME Sanitizer):
//      聚焦远控窗口时无感将输入线程布局切换为纯美式英文 (ENG 0409)，防止本地
//      输入法候选框截断远程按键；离开远控窗口时无感还原原输入法状态。
// ─────────────────────────────────────────────────────────────────────────────

#ifndef TOOLS3000_CORE_REMOTE_REMOTEMASTERENGINE_H
#define TOOLS3000_CORE_REMOTE_REMOTEMASTERENGINE_H

#include "core/utils/Export.h"

#include <windows.h>
#include <string>
#include <vector>
#include <mutex>
#include <atomic>
#include <nlohmann/json.hpp>

namespace tools3000::core {

/// 远程协助主控增强配置结构体
struct TOOLS3000CORE_API RemoteMasterSettings {
    bool enabled = true;                     // 远程主控增强总开关
    bool hotkeyTunnelEnabled = true;         // 沉浸式系统热键直通
    bool emergencyFlushEnabled = true;       // 修饰键卡死急救
    bool doubleRightCtrlTrigger = false;     // 双击右 Ctrl 快捷急救 (默认关闭，避免与聚光灯双击 Ctrl 冲突)
    bool imeSanitizerEnabled = true;         // 远控输入法智能脱敏
    std::string emergencyShortcut = "Ctrl+Alt+Backspace"; // 专用急救快捷键
    std::vector<std::string> targetProcesses = {
        "ToDesk.exe",
        "SunloginClient.exe",
        "AnyDesk.exe",
        "RustDesk.exe",
        "mstsc.exe",
        "msrdc.exe",
        "msrdcw.exe",
        "TeamViewer.exe",
        "vncviewer.exe",
        "tv_w32.exe",
        "parsec.exe",
        "Splashtop.exe",
        "UltraViewer_Desktop.exe",
        "vmware.exe",
        "VirtualBoxVM.exe",
        "uuremote.exe",
        "GameViewer.exe",
        "Moonlight.exe"
    };
    std::vector<std::string> targetClasses = {
        "TSSHELLWND",
        "IHWindowClass",
        "OPContainerClass",
        "SunloginMain",
        "SunloginRemote",
        "ToDesk_Main",
        "ToDesk_Remote",
        "anydesk",
        "RustDesk",
        "TeamViewer"
    };

    nlohmann::json toJson() const;
    static RemoteMasterSettings fromJson(const nlohmann::json& j);
};

/// 已解析的专用急救快捷键配置
struct TOOLS3000CORE_API ParsedEmergencyShortcut {
    bool valid = false;
    bool ctrl = false;
    bool alt = false;
    bool shift = false;
    bool win = false;
    UINT vk = 0;
};

/// 远程协助主控单边增强核心引擎
class TOOLS3000CORE_API RemoteMasterEngine {
public:
    static RemoteMasterEngine& instance();

    /// 初始化引擎 (注册前台窗口监控钩子与加载配置)
    bool initialize();

    /// 停止与关闭引擎 (清理钩子并归还冷路径物理内存)
    void shutdown();

    /// 获取当前设置
    RemoteMasterSettings getSettings() const;

    /// 更新设置并持久化保存
    void updateSettings(const RemoteMasterSettings& settings);

    /// 重置为默认设置
    void resetDefaults();

    /// 查询当前前台窗口是否属于受管远程桌面客户端
    bool isRemoteForeground() const;

    /// 获取当前活跃的远控窗口句柄
    HWND getActiveRemoteHwnd() const;

    /// 获取当前活跃的远控进程名
    std::string getActiveRemoteProcess() const;

    /// 查询当前是否处于输入法脱敏英文状态
    bool isImeSanitized() const;

    /// 获取脱敏前暂存的输入法布局 (HKL)
    HKL getSavedHkl() const;

    /// 处理底层键盘钩子事件 (由 KeyboardHook 优先调用)
    /// @return true 表示消费并拦截按键，阻止本地 Windows Shell 接收
    bool onLowLevelKeyboardEvent(const KBDLLHOOKSTRUCT& data, WPARAM wParam);

    /// 执行全量 8 组修饰键及按需鼠标按键释放冲刷
    /// @param targetHwnd 指定目标远控窗口；为空则自动定位当前或最近活跃远控窗口
    /// @param manualRescue 为 true 表示由设置页/托盘菜单手动触发急救，不因当前前台为宿主窗口而跳过
    void flushModifiers(HWND targetHwnd = nullptr, bool manualRescue = false);

    /// 测试用：模拟前台窗口状态切换
    void setMockForeground(HWND hwnd, bool isRemote, const std::string& processName = "");
    void clearMockForeground();

    // ── 纯函数算法与消息构建器 (高可测性接口) ───────────────────────────

    /// 判定指定进程名与类名是否匹配目标远程桌面客户端列表
    static bool isRemoteProcessOrClass(
        const std::wstring& processName,
        const std::wstring& className,
        const std::vector<std::string>& targetProcesses,
        const std::vector<std::string>& targetClasses
    );

    /// 构建修饰键卡死急救的原子化 SendInput 数组 (8 组修饰键 + 3 组鼠标键)
    static std::vector<INPUT> buildEmergencyFlushInputs();

    /// 解析急救快捷键定义字符串 (如 "Ctrl+Alt+Backspace", "Ctrl+Alt+End", "Ctrl+Shift+F12")
    static ParsedEmergencyShortcut parseEmergencyShortcut(const std::string& str);

    /// 获取当前生效的已解析急救快捷键
    ParsedEmergencyShortcut getParsedEmergencyShortcut() const;

    /// 构建键盘消息的 lParam 参数 (扫描码、扩展键、前后状态位)
    static LPARAM makeKeyLParam(
        DWORD scanCode,
        bool isExtended,
        bool isKeyUp,
        bool isAltDown,
        bool isRepeat = false
    );

private:
    RemoteMasterEngine();
    ~RemoteMasterEngine() = default;
    RemoteMasterEngine(const RemoteMasterEngine&) = delete;
    RemoteMasterEngine& operator=(const RemoteMasterEngine&) = delete;

    void loadSettings();
    static void saveSettings(const RemoteMasterSettings& snapshot);
    void updateParsedEmergencyShortcut();
    void syncAtomicSettingsLocked() noexcept;

    void onRemoteForegroundGained(HWND hwnd);
    void onRemoteForegroundLost(HWND hwnd);
    void postKeyToTarget(HWND fg, UINT msg, DWORD vk, DWORD scanCode, bool isExtended, bool isAltDown = false);
    bool checkWindowIsRemote(HWND hwnd, std::string* outProcessName = nullptr);

    static void CALLBACK winEventProc(
        HWINEVENTHOOK hWinEventHook,
        DWORD event,
        HWND hwnd,
        LONG idObject,
        LONG idChild,
        DWORD idEventThread,
        DWORD dwmsEventTime
    );

    mutable std::recursive_mutex m_mutex;
    RemoteMasterSettings m_settings;
    ParsedEmergencyShortcut m_parsedEmergencyShortcut;
    HWINEVENTHOOK m_foregroundHook = nullptr;

    // 无锁热路径镜像：供 1000Hz WH_KEYBOARD_LL 与 WinEvent 回调零锁读取
    std::atomic<bool> m_enabledAtomic{true};
    std::atomic<bool> m_hotkeyTunnelAtomic{true};
    std::atomic<bool> m_emergencyFlushAtomic{true};
    std::atomic<bool> m_doubleRightCtrlAtomic{false};
    std::atomic<bool> m_imeSanitizerAtomic{true};
    std::atomic<uint64_t> m_packedEmergencyShortcut{0};

    std::atomic<bool> m_isRemoteForeground{false};
    std::atomic<HWND> m_activeRemoteHwnd{nullptr};
    std::atomic<HWND> m_lastRemoteHwnd{nullptr};
    std::atomic<HWND> m_pendingProbeHwnd{nullptr};
    HWND m_lastActiveRemoteHwnd = nullptr;
    std::string m_activeRemoteProcess;

    // 输入法智能脱敏状态
    std::atomic<bool> m_imeSanitized{false};
    bool m_imeClosedViaControl = false;
    HKL m_savedHkl = nullptr;

    // 沉浸热键直通状态机
    std::atomic<bool> m_winKeyDown{false};

    // 双击右 Ctrl 急救检测状态
    std::atomic<ULONGLONG> m_lastRightCtrlUpTime{0};
    std::atomic<bool> m_otherKeyPressedSinceRightCtrl{false};

    // 前台窗口缓存加速
    HWND m_cachedForegroundHwnd = nullptr;
    bool m_cachedIsRemote = false;
    std::string m_cachedProcessName;

    // 测试模拟注入支持
    std::atomic<bool> m_mockActive{false};
    std::atomic<HWND> m_mockHwnd{nullptr};
    std::atomic<bool> m_mockIsRemote{false};
    std::string m_mockProcessName;
};

} // namespace tools3000::core

#endif // TOOLS3000_CORE_REMOTE_REMOTEMASTERENGINE_H
