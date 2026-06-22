#pragma once

#ifndef TOOLS3000_CORE_PLUGIN_PLUGINMANAGER_H
#define TOOLS3000_CORE_PLUGIN_PLUGINMANAGER_H

#include "core/utils/Export.h"
#include "core/plugin/IPlugin.h"
#include "core/plugin/PluginManifest.h"
#include <filesystem>
#include <mutex>
#include <string>
#include <vector>

#ifdef _WIN32
#include <windows.h>
#endif

namespace tools3000::core {

struct PluginInstance {
    std::string id;
    std::string name;
    std::string version;
    std::string fileName;
    std::filesystem::path path;
    PluginManifest manifest;
    IPlugin* plugin = nullptr;
    bool configuredEnabled = true;
    bool enabledAtLaunch = true;
    bool initialized = false;
    std::string error;
#ifdef _WIN32
    HMODULE handle = nullptr;
#else
    void* handle = nullptr;
#endif
};

/// 面向 UI/诊断接口的只读插件快照，不暴露 DLL 句柄和插件对象指针。
struct PluginStatus {
    std::string id;
    std::string name;
    std::string version;
    std::string fileName;
    std::uint32_t abiVersion = 0;
    std::vector<std::string> capabilities;
    std::vector<std::string> permissions;
    std::string executionModel;
    bool enabled = true;
    bool active = false;
    bool restartRequired = false;
    std::string state;
    std::string error;
};

class TOOLS3000CORE_API PluginManager {
public:
    static PluginManager& instance();

    /// 从指定目录扫描并加载所有以 Plugin_ 开头的 DLL
    bool loadPlugins(const std::string& directory);

    /// 初始化所有已加载的插件
    void initializePlugins();

    /// 卸载所有插件
    void shutdownPlugins();

    /// 返回稳定排序的插件运行状态快照。
    std::vector<PluginStatus> getPluginStatuses() const;

    /// 持久化插件开关；若启用尚未加载的插件则当场执行动态热加载与初始化。
    bool setPluginEnabled(const std::string& id, bool enabled, bool& restartRequired,
                          std::string& error);

    /// 查询插件本次启动时是否获准加载。
    bool isEnabledAtLaunch(const std::string& id) const;

    /// 启动期自动对齐并自愈历史分裂的插件配置单一事实源
    void reconcilePluginConfigs();

private:
    PluginManager() = default;
    ~PluginManager() = default;

    bool loadPluginBinaryLocked(PluginInstance& inst);
    bool initializeSinglePluginLocked(PluginInstance& inst);

    mutable std::mutex m_mutex;
    bool m_shuttingDown = false;
    std::vector<PluginInstance> m_plugins;
};

} // namespace tools3000::core

#endif // TOOLS3000_CORE_PLUGIN_PLUGINMANAGER_H
