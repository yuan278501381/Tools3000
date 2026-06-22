#pragma once

#ifndef TOOLS3000_CORE_PLUGIN_IPLUGIN_H
#define TOOLS3000_CORE_PLUGIN_IPLUGIN_H

#include "core/utils/Export.h"
#include "core/plugin/PluginManifest.h"

namespace tools3000::core {

class IPlugin {
public:
    virtual ~IPlugin() = default;

    /// 获取插件的唯一名称
    virtual const char* getName() const = 0;

    /// 获取插件的版本
    virtual const char* getVersion() const = 0;

    /// 初始化插件
    virtual bool initialize() = 0;

    /// 卸载插件
    virtual void shutdown() = 0;
};

} // namespace tools3000::core

// 每个插件 DLL 必须实现并导出此函数
// extern "C" __declspec(dllexport) tools3000::core::IPlugin* CreatePlugin();
// extern "C" __declspec(dllexport) std::uint32_t GetPluginAbiVersion();

#endif // TOOLS3000_CORE_PLUGIN_IPLUGIN_H
