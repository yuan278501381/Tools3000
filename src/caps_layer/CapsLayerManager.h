#pragma once
// ─────────────────────────────────────────────────────────────────────────────
// CapsLayerManager.h — CapsLock 引导键与分应用改键管理器
//
// 架构特性:
//   1. 出厂默认关闭：关闭态下仅 1 次无锁 atomic 读取 (< 1ns) 短路，0 线程、0 堆分配。
//   2. 纯 Rust 零依赖状态机静态库 (tools3000_caps_rs.lib) 负责滚键防粘滞与多层裁决。
//   3. 零阻塞防连坐：
//      - 复用宿主既有唯一 WH_KEYBOARD_LL，绝不挂载第二条底层键盘钩子；
//      - 钩子热路径使用 try_lock + 预先缓存的前台进程名 (0 OpenProcess)；
//      - 外层 try/catch + 连续异常熔断器，发生故障自动熔断降级绝不拖垮主程序；
//      - 组合键/短按/Unicode 文本展开投递至按需拉起的轻量后台工作线程执行，
//        全部 SendInput 携带独立标签 kCapsLayerInjectedTag 防自触发递归。
// ─────────────────────────────────────────────────────────────────────────────

#ifndef TOOLS3000_CAPS_LAYER_MANAGER_H
#define TOOLS3000_CAPS_LAYER_MANAGER_H

#include <windows.h>

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <nlohmann/json.hpp>

struct CapsEngine;

namespace tools3000::caps_layer {

/// 注入事件防递归标记 ("T3KC")
inline constexpr ULONG_PTR kCapsLayerInjectedTag = 0x54334B43u;

/// 修饰键位掩码（禁止使用 MOD_CTRL 等 Windows 系统宏名，防止 C2059 冲突）
inline constexpr uint8_t CAPS_MOD_CTRL = 0x01;
inline constexpr uint8_t CAPS_MOD_ALT = 0x02;
inline constexpr uint8_t CAPS_MOD_SHIFT = 0x04;
inline constexpr uint8_t CAPS_MOD_WIN = 0x08;

class CapsLayerManager {
public:
    static CapsLayerManager& instance();

    /// 初始化：从 ConfigManager 读取 /capsLayer 配置（默认关闭）；关闭时不创建后台线程
    void initialize();

    /// 安全停机：停止后台投递线程并释放 Rust 状态机实例
    void shutdown();

    /// 零开销热路径开关判断
    bool isEnabled() const noexcept {
        return m_enabled.load(std::memory_order_acquire) &&
               !m_circuitBreakerTripped.load(std::memory_order_acquire);
    }

    /// 启停开关（同步持久化至 /capsLayer/enabled 与 /plugins/caps_layer/enabled）
    bool setEnabled(bool enabled);

    /// 更新前台窗口进程名缓存（由宿主 EVENT_SYSTEM_FOREGROUND 调用，钩子热路径 0 OpenProcess）
    void updateForegroundProcess(const std::string& processName);

    /// 重置运行期按键状态机（锁屏/休眠/进入全屏游戏/进入远控时调用，防粘滞卡键）
    void resetRuntimeState();

    /// 底层键盘钩子入口：返回 true 表示吞掉原按键并已接管映射；外层自带异常熔断零连坐保护
    bool handleLowLevelKey(const KBDLLHOOKSTRUCT& data, WPARAM wParam) noexcept;

    /// 获取当前完整配置 JSON（供 IPC capsLayer.getSettings）
    nlohmann::json getSettingsJson() const;

    /// 局部或全量更新配置（供 IPC capsLayer.updateSettings）
    nlohmann::json updateSettings(const nlohmann::json& params);

    /// 一键套用内置经典布局预设 ("arrows_ijkl" | "arrows_vim" | "arrows_esdf")
    nlohmann::json applyPreset(const std::string& presetId);

    /// 键名 <-> Windows 虚拟键码转换辅助（供单元测试与配置校验复用）
    static uint32_t parseKeyNameToVk(const std::string& keyName);
    static bool parseComboString(const std::string& comboStr, uint8_t& outMods, uint32_t& outVk);
    static nlohmann::json buildDefaultSettingsJson();
    static nlohmann::json buildPresetGlobalMappings(const std::string& presetId);

private:
    CapsLayerManager() = default;
    ~CapsLayerManager();

    CapsLayerManager(const CapsLayerManager&) = delete;
    CapsLayerManager& operator=(const CapsLayerManager&) = delete;

    struct AsyncInjectionTask {
        uint8_t kind = 0;  // 3=SendCombo, 4=SendText, 5=TapToggleCaps, 6=TapEscape
        uint8_t mods = 0;
        uint32_t vk = 0;
        std::wstring textW;
    };

    void ensureEngineLocked();
    void loadFromConfigLocked();
    void saveToConfigLocked() const;
    void rebuildRustRulesLocked();
    void startInjectorWorkerLocked();
    void stopInjectorWorker();
    void injectorWorkerLoop();
    void enqueueAsyncTask(AsyncInjectionTask task);
    void pushStateChangedEvents(bool enabled) const;

    static bool isExtendedVirtualKey(uint32_t vk) noexcept;
    static INPUT makeKeyboardInput(uint32_t vk, bool isKeyUp) noexcept;
    static void sendSingleKeyImmediate(uint32_t vk, bool isKeyUp) noexcept;
    static void executeAsyncTask(const AsyncInjectionTask& task) noexcept;

    mutable std::mutex m_stateMutex;
    nlohmann::json m_settings = nlohmann::json::object();
    bool m_initialized = false;

    mutable std::mutex m_engineMutex;
    CapsEngine* m_engine = nullptr;

    mutable std::mutex m_fgMutex;
    std::string m_cachedFgExeLower;

    std::atomic<bool> m_enabled{false};
    std::atomic<bool> m_capsOrRolloverActive{false};
    std::atomic<bool> m_circuitBreakerTripped{false};
    std::atomic<uint32_t> m_consecutiveHookErrors{0};

    std::mutex m_workerLifecycleMutex;
    std::thread m_injectorThread;
    std::mutex m_taskMutex;
    std::condition_variable m_taskCv;
    std::deque<AsyncInjectionTask> m_taskQueue;
    std::atomic<bool> m_workerRunning{false};
};

}  // namespace tools3000::caps_layer

#endif  // TOOLS3000_CAPS_LAYER_MANAGER_H
