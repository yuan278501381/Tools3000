#pragma once
// ─────────────────────────────────────────────────────────────────────────────
// ConfigManager — 配置管理器
//
// 特性:
//   1. JSON 格式持久化（基于 nlohmann::json）
//   2. 文件变更监控 + 热加载（ReadDirectoryChangesW）
//   3. 类型安全的 get/set 接口
//   4. 默认值回退机制
//   5. 配置变更通知（观察者模式）
// ─────────────────────────────────────────────────────────────────────────────

#ifndef TOOLS3000_CORE_CONFIG_CONFIGMANAGER_H
#define TOOLS3000_CORE_CONFIG_CONFIGMANAGER_H

#include "core/utils/Export.h"

#include <string>
#include <string_view>
#include <filesystem>
#include <mutex>
#include <functional>
#include <vector>
#include <optional>
#include <atomic>
#include <thread>
#include <windows.h>

#include <nlohmann/json.hpp>

namespace tools3000::core {

using json = nlohmann::json;
using ConfigChangeCallback = std::function<void(const std::string& key)>;

class TOOLS3000CORE_API ConfigManager {
public:
    /// 获取单例实例
    static ConfigManager& instance();

    /// 初始化配置管理器
    /// @param configDir 配置文件所在目录
    /// @param filename 配置文件名（默认 "config.json"）
    bool initialize(const std::filesystem::path& configDir, const std::string& filename = "config.json");

    /// 关闭（停止文件监控线程）
    void shutdown();

    // ── 类型安全的读写接口 ───────────────────────────────────────────────

    /// 获取配置值（支持 JSON Pointer 路径，如 "/gesture/trigger/button"）
    /// @tparam T 目标类型
    /// @param key JSON Pointer 路径
    /// @param defaultValue 当 key 不存在时的回退值
    template <typename T>
    T get(const std::string& key, const T& defaultValue = T{}) const {
        std::lock_guard lock(m_mutex);
        try {
            auto ptr = json::json_pointer(key);
            if (m_config.contains(ptr)) {
                return m_config.at(ptr).get<T>();
            }
        } catch (...) {
            // JSON Pointer 解析失败或类型不匹配，返回默认值
        }
        return defaultValue;
    }

    /// 设置配置值并自动保存到文件
    template <typename T>
    bool set(const std::string& key, const T& value) {
        return setJsonValue(key, json(value));
    }

    /// 以 RFC 7396 merge-patch 方式一次提交多个配置项，只触发一次落盘。
    bool mergePatch(const json& patch, const std::string& notificationKey = "*");

    /// 检查 key 是否存在
    bool has(const std::string& key) const;

    /// 删除指定 key
    bool remove(const std::string& key);

    /// 获取完整配置的 JSON 字符串（用于 IPC 传输给前端）
    std::string toJsonString(int indent = 2) const;

    /// 从 JSON 字符串批量更新（用于前端 IPC 回写）
    bool fromJsonString(const std::string& jsonStr);

    /// 清空当前配置并立即持久化。调用方随后会回退到各模块的默认值。
    bool reset();

    // ── 配置变更通知 ─────────────────────────────────────────────────────

    /// 注册变更回调（返回回调 ID，用于取消注册）
    size_t onChange(ConfigChangeCallback callback);

    /// 取消注册变更回调
    void removeOnChange(size_t callbackId);

    // ── 导入/导出 ────────────────────────────────────────────────────────

    /// 导出配置到文件
    bool exportTo(const std::filesystem::path& filePath) const;

    /// 从文件导入配置（合并模式，不会清除未包含的 key）
    bool importFrom(const std::filesystem::path& filePath);

    ~ConfigManager();

private:
    ConfigManager() = default;
    ConfigManager(const ConfigManager&) = delete;
    ConfigManager& operator=(const ConfigManager&) = delete;

    /// 从文件加载配置
    bool load(bool* changed = nullptr);

    /// 保存配置到文件
    bool save() const;

    /// m_ioMutex 已持有时，将快照原子写入配置文件。
    bool writeSnapshotLocked(const json& snapshot) const;

    bool setJsonValue(const std::string& key, json value);

    /// 通知所有观察者
    void notifyChange(const std::string& key);

    /// 文件变更监控线程函数
    void watchFileChanges();

    json m_config;
    std::filesystem::path m_configFilePath;
    mutable std::mutex m_mutex;
    mutable std::mutex m_ioMutex;

    // 变更通知
    std::vector<std::pair<size_t, ConfigChangeCallback>> m_callbacks;
    std::atomic<size_t> m_nextCallbackId{0};

    // 文件监控
    std::thread m_watchThread;
    std::atomic<bool> m_watchRunning{false};
    HANDLE m_watchStopEvent = nullptr;
};

}  // namespace tools3000::core

#endif  // TOOLS3000_CORE_CONFIG_CONFIGMANAGER_H
