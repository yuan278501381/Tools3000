/**
 * Tools3000 - High Performance Windows Productivity Suite
 * 
 * Copyright (c) 2026 Yy1 (GitHub yuan278501381) <https://github.com/yuan278501381> & Tools3000 contributors
 * 
 * Licensed under the MIT License.
 */

#include "core/plugin/IPlugin.h"
#include "Tools3000Version.h"
#include "core/logger/Logger.h"
#include "core/ipc/MessageBridge.h"
#include "core/config/ConfigManager.h"
#include "DialogEngine.h"
#include "PathMemoryManager.h"
#include "ExplorerTracker.h"
#include "DialogNavigator.h"
#include <windows.h>
#include <nlohmann/json.hpp>

using json = nlohmann::json;

namespace tools3000::dialog {

class DialogEnhancerPlugin : public tools3000::core::IPlugin {
public:
    const char* getName() const override { return "DialogEnhancer"; }
    const char* getVersion() const override { return tools3000::version::String; }

    bool initialize() override {
        LOG_INFO("初始化文件对话框增强插件 (DialogEnhancerPlugin)");

        if (PathMemoryManager::instance().isEnabled()) {
            if (!DialogEngine::instance().start()) {
                LOG_ERROR("启动 DialogEngine 失败");
                return false;
            }
        }

        registerIpcHandlers();
        return true;
    }

    void shutdown() override {
        LOG_INFO("关闭文件对话框增强插件 (DialogEnhancerPlugin)");
        unregisterIpcHandlers();
        DialogEngine::instance().stop();
    }

private:
    void registerIpcHandlers() {
        auto& bridge = tools3000::core::MessageBridge::instance();

        bridge.registerHandler("dialog.getConfig", [](const json&) -> json {
            auto& mgr = PathMemoryManager::instance();
            return {
                {"enabled", mgr.isEnabled()},
                {"perAppMemory", mgr.isPerAppMemoryEnabled()},
                {"quickSwitch", mgr.isQuickSwitchEnabled()},
                {"ribbonEnabled", mgr.isRibbonEnabled()},
                {"ribbonPosition", mgr.getRibbonPosition()}
            };
        });

        bridge.registerHandler("dialog.updateConfig", [](const json& params) -> json {
            auto& mgr = PathMemoryManager::instance();
            if (params.contains("enabled")) {
                bool enabled = params["enabled"].get<bool>();
                mgr.setEnabled(enabled);
                if (enabled) {
                    DialogEngine::instance().start();
                } else {
                    DialogEngine::instance().stop();
                }
                tools3000::core::ConfigManager::instance().set<bool>("/dialog/enabled", enabled);
                tools3000::core::ConfigManager::instance().set<bool>("/plugins/dialogenhancer/enabled", enabled);
                tools3000::core::ConfigManager::instance().set<bool>("/plugins/dialog_enhancer/enabled", enabled);
                tools3000::core::MessageBridge::instance().pushEvent("dialog:configChanged", {{"enabled", enabled}});
                tools3000::core::MessageBridge::instance().pushEvent("plugins:changed", {{"id", "dialogenhancer"}, {"enabled", enabled}});
            }
            if (params.contains("perAppMemory")) mgr.setPerAppMemoryEnabled(params["perAppMemory"].get<bool>());
            if (params.contains("quickSwitch")) mgr.setQuickSwitchEnabled(params["quickSwitch"].get<bool>());
            if (params.contains("ribbonEnabled")) mgr.setRibbonEnabled(params["ribbonEnabled"].get<bool>());
            if (params.contains("ribbonPosition")) mgr.setRibbonPosition(params["ribbonPosition"].get<std::string>());
            return {{"success", true}};
        });

        bridge.registerHandler("dialog.getRecentPaths", [](const json&) -> json {
            auto paths = PathMemoryManager::instance().getRecentPaths(20);
            return paths;
        });

        bridge.registerHandler("dialog.clearRecentPaths", [](const json&) -> json {
            PathMemoryManager::instance().clearRecentPaths();
            return {{"success", true}};
        });

        bridge.registerHandler("dialog.getFavorites", [](const json&) -> json {
            auto favs = PathMemoryManager::instance().getFavorites();
            return favs;
        });

        bridge.registerHandler("dialog.setFavorites", [](const json& params) -> json {
            if (params.is_array()) {
                PathMemoryManager::instance().setFavorites(params.get<std::vector<std::string>>());
            } else if (params.is_object() && params.contains("favorites")) {
                PathMemoryManager::instance().setFavorites(params["favorites"].get<std::vector<std::string>>());
            }
            return {{"success", true}};
        });

        bridge.registerHandler("dialog.addFavorite", [](const json& params) -> json {
            if (params.is_string()) {
                PathMemoryManager::instance().addFavorite(params.get<std::string>());
            } else if (params.is_object() && params.contains("path")) {
                PathMemoryManager::instance().addFavorite(params["path"].get<std::string>());
            }
            return {{"success", true}};
        });

        bridge.registerHandler("dialog.removeFavorite", [](const json& params) -> json {
            if (params.is_string()) {
                PathMemoryManager::instance().removeFavorite(params.get<std::string>());
            } else if (params.is_object() && params.contains("path")) {
                PathMemoryManager::instance().removeFavorite(params["path"].get<std::string>());
            }
            return {{"success", true}};
        });

        bridge.registerHandler("dialog.getAppMemories", [](const json&) -> json {
            auto list = PathMemoryManager::instance().getAllAppMemories();
            json arr = json::array();
            for (const auto& item : list) {
                arr.push_back({
                    {"processName", item.processName},
                    {"lastPath", item.lastPath},
                    {"fixedWorkspace", item.fixedWorkspace},
                    {"isFixed", item.isFixed},
                    {"lastUsedTimestamp", item.lastUsedTimestamp}
                });
            }
            return arr;
        });

        bridge.registerHandler("dialog.setAppFixedWorkspace", [](const json& params) -> json {
            if (params.is_object() && params.contains("processName")) {
                std::string proc = params["processName"].get<std::string>();
                std::string path = params.value("workspacePath", "");
                bool isFixed = params.value("isFixed", false);
                PathMemoryManager::instance().setAppFixedWorkspace(proc, path, isFixed);
            }
            return {{"success", true}};
        });

        bridge.registerHandler("dialog.removeAppMemory", [](const json& params) -> json {
            if (params.is_string()) {
                PathMemoryManager::instance().removeAppMemory(params.get<std::string>());
            } else if (params.is_object() && params.contains("processName")) {
                PathMemoryManager::instance().removeAppMemory(params["processName"].get<std::string>());
            }
            return {{"success", true}};
        });

        bridge.registerHandler("dialog.clearAppMemories", [](const json&) -> json {
            PathMemoryManager::instance().clearAppMemories();
            return {{"success", true}};
        });

        bridge.registerHandler("dialog.getBlacklist", [](const json&) -> json {
            return PathMemoryManager::instance().getBlacklist();
        });

        bridge.registerHandler("dialog.setBlacklist", [](const json& params) -> json {
            if (params.is_array()) {
                PathMemoryManager::instance().setBlacklist(params.get<std::vector<std::string>>());
            } else if (params.is_object() && params.contains("blacklist")) {
                PathMemoryManager::instance().setBlacklist(params["blacklist"].get<std::vector<std::string>>());
            }
            return {{"success", true}};
        });

        bridge.registerHandler("dialog.getActiveExplorerPath", [](const json&) -> json {
            std::string path = ExplorerTracker::instance().getActiveExplorerPath();
            return {{"path", path}};
        });
    }

    void unregisterIpcHandlers() {
        auto& bridge = tools3000::core::MessageBridge::instance();
        bridge.unregisterHandlersByPrefix("dialog.");
    }
};

extern "C" __declspec(dllexport) tools3000::core::IPlugin* CreatePlugin() {
    static DialogEnhancerPlugin s_instance;
    return &s_instance;
}

extern "C" __declspec(dllexport) std::uint32_t GetPluginAbiVersion() {
    return tools3000::core::CurrentPluginAbiVersion;
}

} // namespace tools3000::dialog

// 全局保存本 DLL 的 HMODULE，供 SetWinEventHook 使用
// SetWinEventHook 从 DLL 调用时必须传入本 DLL 的 hModule，否则返回 ERROR_HOOK_NEEDS_HMOD(1426)
HMODULE g_hDialogEnhancerModule = nullptr;

BOOL APIENTRY DllMain(HMODULE hModule, DWORD ul_reason_for_call, LPVOID) {
    switch (ul_reason_for_call) {
        case DLL_PROCESS_ATTACH:
            DisableThreadLibraryCalls(hModule);
            g_hDialogEnhancerModule = hModule;  // 保存供 SetWinEventHook 使用
            break;
        case DLL_PROCESS_DETACH:
            g_hDialogEnhancerModule = nullptr;
            break;
    }
    return TRUE;
}
