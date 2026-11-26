#pragma once
// ─────────────────────────────────────────────────────────────────────────────
// WindowKitController.h — 纯 Rust 独立窗口管理引擎 (Tools3000_WindowKit.exe) 宿主控制器
//
// 核心工程铁律:
//   1. 出厂默认全关零占用：enabled==false 或全部 8 个子开关为 false 时绝不启动子进程
//      (0 进程 / 0 MB / 0 线程 / 0 热键)。
//   2. 阻塞与崩溃零连坐：
//      - 宿主与 WindowKit 日常通信 100% 走异步 PostMessageW，严禁 SendMessageW 同步阻塞；
//      - 子进程纳入宿主 Job Object (JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE)，主进程退出毫秒回收；
//      - 崩溃退避自愈熔断器：30 秒内意外退出 > 2 次自动停止重启并降级，绝不死循环拖垮宿主；
//      - One-shot CLI 探测 (显示器/捕获工作区/导入 PowerToys) 严格带有限超时保护；
//      - 提权且已签名正式宿主下，强制校验 WindowKit 二进制 Authenticode 签名防篡改。
// ─────────────────────────────────────────────────────────────────────────────

#ifndef TOOLS3000_CORE_IPC_WINDOWKITCONTROLLER_H
#define TOOLS3000_CORE_IPC_WINDOWKITCONTROLLER_H

#include "core/utils/Export.h"

#include <windows.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <filesystem>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <nlohmann/json.hpp>

namespace tools3000::core {

/// 与 src/window_kit/src/core/constants.rs 1:1 精确对齐的控制窗口消息与动作码
inline constexpr wchar_t kWindowKitExeName[] = L"Tools3000_WindowKit.exe";
inline constexpr wchar_t kWindowKitControlClassName[] = L"Tools3000_WindowKit_Control";
inline constexpr wchar_t kWindowKitEditorResultMsgName[] = L"Tools3000_WindowKit_EditorResult";

inline constexpr UINT kWindowKitMsgReloadConfig = WM_APP + 1;
inline constexpr UINT kWindowKitMsgExecAction = WM_APP + 2;
inline constexpr UINT kWindowKitMsgSetMutedFlags = WM_APP + 3;
inline constexpr UINT kWindowKitMsgGracefulShutdown = WM_APP + 4;

inline constexpr WPARAM kWindowKitActionToggleAlwaysOnTop = 1;
inline constexpr WPARAM kWindowKitActionOpacityUp = 2;
inline constexpr WPARAM kWindowKitActionOpacityDown = 3;
inline constexpr WPARAM kWindowKitActionOpacityReset = 4;
inline constexpr WPARAM kWindowKitActionMoveNextZone = 5;
inline constexpr WPARAM kWindowKitActionMovePrevZone = 6;
inline constexpr WPARAM kWindowKitActionMoveZoneIndex = 7;
inline constexpr WPARAM kWindowKitActionRestoreFromZone = 8;
inline constexpr WPARAM kWindowKitActionRescueOffscreen = 9;
inline constexpr WPARAM kWindowKitActionMoveNextMonitor = 10;
inline constexpr WPARAM kWindowKitActionMinimizeToTray = 11;
inline constexpr WPARAM kWindowKitActionRestoreAllTray = 12;
inline constexpr WPARAM kWindowKitActionOpenQuickSwitch = 13;
inline constexpr WPARAM kWindowKitActionCaptureWorkspace = 14;
inline constexpr WPARAM kWindowKitActionLaunchWorkspace = 15;
inline constexpr WPARAM kWindowKitActionImportPowerToysZones = 16;
inline constexpr WPARAM kWindowKitActionQueryMonitors = 17;
inline constexpr WPARAM kWindowKitActionQueryRuntimeStats = 18;
inline constexpr WPARAM kWindowKitActionShowZonesPreview = 19;
inline constexpr WPARAM kWindowKitActionOpenLayoutEditor = 20;

class TOOLS3000CORE_API WindowKitController {
public:
    static WindowKitController& instance();

    /// 初始化：读取 /windowManager 配置；默认全关时不拉起任何进程或线程
    void initialize();

    /// 安全停机：发送 WM_APP_GRACEFUL_SHUTDOWN 还原全部托盘/置顶/透明窗口并回收子进程
    void shutdown();

    /// 是否已启用且至少开启 1 个子功能
    bool isRunningDesired() const noexcept;

    /// 检查二进制文件是否存在、无重解析点劫持、且在提权签名宿主下具备有效数字签名
    bool isBinaryInstalled() const;

    /// 获取前端标准 WindowManagerResponse JSON（供 IPC windowManager.getSettings）
    nlohmann::json getSettingsJson() const;

    /// 更新配置并按需启动/热重载/优雅停止子进程（供 IPC windowManager.updateSettings）
    nlohmann::json updateSettings(const nlohmann::json& params);

    /// 总开关快捷设置（供 plugins.setEnabled("window_manager") 调用）
    bool setMasterEnabled(bool enabled, std::string& error);

    /// 异步派发控制动作（PostMessageW 零阻塞；供 IPC windowManager.execAction 及全局热键复用）
    nlohmann::json execAction(const std::string& actionName, const nlohmann::json& arg = nullptr);

    /// 同步全屏游戏免打扰 / 远程桌面沉浸直通状态位（仅在状态变化且进程活跃时 PostMessageW）
    void syncMutedFlags(bool fullscreenMuted, bool remoteMuted);

    /// 同步宿主外观主题、语言或日志级别变更
    void notifyHostEnvironmentChanged();

    /// 查询多显示器拓扑信息（有限超时 one-shot CLI，附带当前 Windows 虚拟桌面列表）
    nlohmann::json getMonitorsJson();

    /// 读取 Windows 注册表枚举全部虚拟桌面 GUID、用户自定义名称与当前活跃桌面
    nlohmann::json getVirtualDesktopsJson() const;

    /// 消费全屏布局编辑器保存回写文件并持久化到 ConfigManager、热重载 Rust 与推送前端刷新
    bool handleLayoutEditorResult(bool saved);

    /// 捕获当前桌面所有应用窗口为工作区快照（有限超时 one-shot CLI）
    nlohmann::json captureWorkspaceJson(const std::string& name = "");

    /// 一键导入 PowerToys FancyZones 自定义布局（有限超时 one-shot CLI）
    nlohmann::json importPowerToysLayoutsJson(const std::string& sourcePath = "");

    /// 零阻塞查询运行期内存与句柄统计（直接通过进程句柄读取，绝不阻塞等待子进程响应）
    nlohmann::json getRuntimeStatsJson() const;

    /// 供 Alt+Space 搜索框枚举已启用的场景工作区
    nlohmann::json listSearchableWorkspacesJson() const;

    /// 构建出厂默认 WindowManagerSettings JSON（全开关默认 false + 内置分区预设）
    static nlohmann::json buildDefaultSettingsJson();

    /// 判断给定配置对象是否需要拉起子进程（enabled && 至少 1 个 features.* 为 true）
    static bool hasAnyActiveFeature(bool masterEnabled, const nlohmann::json& settings) noexcept;

    /// 将动作名（支持 snake_case 与 camelCase）映射为 WindowKit 1..19 控制消息码
    static WPARAM resolveActionCode(const std::string& actionName) noexcept;

private:
    WindowKitController() = default;
    ~WindowKitController();

    WindowKitController(const WindowKitController&) = delete;
    WindowKitController& operator=(const WindowKitController&) = delete;

    std::filesystem::path resolveBinaryPath() const;
    std::filesystem::path resolveRuntimeConfigPath() const;
    std::filesystem::path resolveActionPayloadPath() const;
    std::filesystem::path resolveEditorResultPath() const;

    void loadFromConfigLocked();
    void saveToConfigLocked() const;
    nlohmann::json buildResponseJsonLocked() const;
    nlohmann::json buildRuntimeStatsLocked() const;
    nlohmann::json buildRustRuntimePayloadLocked(const std::string& traceId) const;
    bool writeRuntimeConfigAtomicLocked(const std::string& traceId) const;

    void reconcileProcessStateLocked(bool allowResetBreaker = false);
    bool launchManagedProcessLocked(const std::string& traceId);
    void stopManagedProcessLocked(DWORD gracefulTimeoutMs);
    bool isProcessAliveLocked() const noexcept;
    HWND findManagedControlWindowLocked() const noexcept;
    void postControlMessageLocked(UINT msg, WPARAM wp, LPARAM lp) const noexcept;

    void startWatchdogLocked();
    void stopWatchdog();
    void watchdogLoop();

    void syncHotkeysLocked();
    void unregisterAllHotkeys();
    void pushStateChangedEvents() const;

    nlohmann::json runBoundedOneShotCli(
        const std::wstring& args,
        const std::filesystem::path& outputFile,
        DWORD timeoutMs = 4500) const;

    mutable std::mutex m_mutex;
    bool m_masterEnabled = false;
    nlohmann::json m_settings = nlohmann::json::object();
    bool m_initialized = false;

    HANDLE m_processHandle = nullptr;
    DWORD m_processId = 0;

    std::deque<std::chrono::steady_clock::time_point> m_recentCrashTimes;
    bool m_circuitBreakerTripped = false;
    uint32_t m_restartCount = 0;

    std::atomic<bool> m_shouldRun{false};
    std::atomic<uint32_t> m_mutedFlags{0};
    std::atomic<bool> m_shuttingDown{false};

    std::mutex m_watchdogLifecycleMutex;
    std::thread m_watchdogThread;
    std::mutex m_watchdogCvMutex;
    std::condition_variable m_watchdogCv;
    std::atomic<bool> m_watchdogRunning{false};
    /// 上一次实际注册到 HotkeyManager 的热键快照，避免滑块/参数微调触发冗余注销重注册刷屏
    std::map<std::string, std::string> m_registeredHotkeys;
};

}  // namespace tools3000::core

#endif  // TOOLS3000_CORE_IPC_WINDOWKITCONTROLLER_H
