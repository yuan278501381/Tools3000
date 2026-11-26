/**
 * Tools3000 - High Performance Windows Productivity Suite
 *
 * Copyright (c) 2026 Yy1 (GitHub yuan278501381) <https://github.com/yuan278501381> & Tools3000 contributors
 *
 * Licensed under the GPL-3.0 License.
 */

#include "core/plugin/IPlugin.h"
#include "Tools3000Version.h"
#include "core/logger/Logger.h"
#include "core/ipc/MessageBridge.h"
#include "core/config/ConfigManager.h"
#include "core/events/EventBus.h"
#include "core/hotkey/HotkeyManager.h"
#include "core/i18n/UiI18n.h"
#include "core/utils/OutOfProcessFileDialog.h"
#include "core/utils/WinUtils.h"
#include "DialogEngine.h"
#include "DialogRibbonOverlay.h"
#include "PathMemoryManager.h"
#include "ExplorerTracker.h"
#include "DialogNavigator.h"

#include <windows.h>
#include <shobjidl.h>
#include <wrl/client.h>
#include <nlohmann/json.hpp>

#include <filesystem>
#include <optional>
#include <string>

using json = nlohmann::json;
using Microsoft::WRL::ComPtr;

namespace tools3000::dialog {

namespace {

// 独立文件夹选择器（通过短命子进程隔离打开，避免外壳扩展与 .NET CLR 常驻宿主进程）
std::optional<std::string> browseDirectoryDialogUtf8() {
    tools3000::core::FileDialogRequest req;
    req.mode = tools3000::core::FileDialogMode::PickFolder;
    req.title = tools3000::core::trW(L"选择常用文件夹", L"Select Favorite Folder");
    const auto res = tools3000::core::OutOfProcessFileDialog::show(req);
    if (!res.accepted || res.selectedPath.empty()) {
        return std::nullopt;
    }
    return tools3000::core::WinUtils::pathToUtf8(res.selectedPath);
}

}  // namespace

class DialogEnhancerPlugin : public tools3000::core::IPlugin {
public:
    const char* getName() const override { return "DialogEnhancer"; }
    const char* getVersion() const override { return tools3000::version::String; }

    bool initialize() override {
        LOG_INFO("初始化文件对话框增强插件 (DialogEnhancerPlugin)");
        PathMemoryManager::instance().loadConfig();

        if (PathMemoryManager::instance().isEnabled()) {
            if (!DialogEngine::instance().start()) {
                LOG_ERROR("启动 DialogEngine 失败");
                return false;
            }
        }

        // 将文件对话框作用域双快捷键纳入 HotkeyManager 统一总览（SSOT）：
        // 先注册空热键再置 armed=false & scope="dialog" 后重绑目标键值，确保绝不占用全局系统热键。
        auto registerScopedDialogHotkey = [](const std::string& name, const std::string& shortcut) {
            auto& hm = tools3000::core::HotkeyManager::instance();
            hm.registerHotkey(name, tools3000::core::HotkeyDef{}, []() {});
            hm.setHotkeyArmed(name, false);
            hm.setHotkeyScope(name, "dialog");
            if (!shortcut.empty()) {
                if (const auto parsed = tools3000::core::HotkeyDef::fromString(shortcut);
                    parsed && parsed->isSafe()) {
                    hm.rebindHotkey(name, *parsed);
                }
            }
        };
        auto& pathMgr = PathMemoryManager::instance();
        registerScopedDialogHotkey("Dialog Quick Jump", pathMgr.getQuickJumpHotkey());
        registerScopedDialogHotkey("Dialog Inline Search", pathMgr.getInlineSearchHotkey());

        m_themeSubId = tools3000::core::EventBus::instance().subscribe<tools3000::core::ThemeChangedEvent>(
            [](const tools3000::core::ThemeChangedEvent&) {
                DialogRibbonOverlay::instance().onThemeChanged();
            });
        m_dialogHotkeysSubId = tools3000::core::EventBus::instance().subscribe<tools3000::core::DialogHotkeysChangedEvent>(
            [](const tools3000::core::DialogHotkeysChangedEvent& ev) {
                auto& mgr = PathMemoryManager::instance();
                if (ev.hasQuickJump && mgr.getQuickJumpHotkey() != ev.quickJumpHotkey) {
                    mgr.setQuickJumpHotkey(ev.quickJumpHotkey);
                }
                if (ev.hasInlineSearch && mgr.getInlineSearchHotkey() != ev.inlineSearchHotkey) {
                    mgr.setInlineSearchHotkey(ev.inlineSearchHotkey);
                }
                DialogEngine::instance().syncScopedHotkeys();
            });

        registerIpcHandlers();
        return true;
    }

    void shutdown() override {
        LOG_INFO("关闭文件对话框增强插件 (DialogEnhancerPlugin)");
        if (m_dialogHotkeysSubId != 0) {
            tools3000::core::EventBus::instance().unsubscribeAndWait(m_dialogHotkeysSubId);
            m_dialogHotkeysSubId = 0;
        }
        if (m_themeSubId != 0) {
            tools3000::core::EventBus::instance().unsubscribeAndWait(m_themeSubId);
            m_themeSubId = 0;
        }
        tools3000::core::HotkeyManager::instance().unregisterHotkey("Dialog Quick Jump");
        tools3000::core::HotkeyManager::instance().unregisterHotkey("Dialog Inline Search");
        unregisterIpcHandlers();
        DialogEngine::instance().stop();
        DialogNavigator::instance().shutdownAsyncWorker();
        ExplorerTracker::instance().shutdown();
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
                {"inlineSearchEnabled", mgr.isInlineSearchEnabled()},
                {"ribbonPosition", mgr.getRibbonPosition()},
                {"quickJumpHotkey", mgr.getQuickJumpHotkey()},
                {"inlineSearchHotkey", mgr.getInlineSearchHotkey()}
            };
        });

        bridge.registerHandler("dialog.updateConfig", [](const json& params) -> json {
            if (!params.is_object()) {
                return {{"success", false}};
            }
            auto& mgr = PathMemoryManager::instance();
            if (params.contains("enabled") && params["enabled"].is_boolean()) {
                const bool enabled = params["enabled"].get<bool>();
                mgr.setEnabled(enabled);
                if (enabled) {
                    DialogEngine::instance().start();
                } else {
                    DialogEngine::instance().stop();
                }
            }
            if (params.contains("perAppMemory") && params["perAppMemory"].is_boolean()) {
                mgr.setPerAppMemoryEnabled(params["perAppMemory"].get<bool>());
            }
            if (params.contains("quickSwitch") && params["quickSwitch"].is_boolean()) {
                mgr.setQuickSwitchEnabled(params["quickSwitch"].get<bool>());
            }
            if (params.contains("ribbonEnabled") && params["ribbonEnabled"].is_boolean()) {
                mgr.setRibbonEnabled(params["ribbonEnabled"].get<bool>());
            }
            if (params.contains("inlineSearchEnabled") && params["inlineSearchEnabled"].is_boolean()) {
                mgr.setInlineSearchEnabled(params["inlineSearchEnabled"].get<bool>());
            }
            if (params.contains("ribbonPosition") && params["ribbonPosition"].is_string()) {
                mgr.setRibbonPosition(params["ribbonPosition"].get<std::string>());
            }
            auto mirrorDialogHotkeyToManager = [](const std::string& hotkeyName, const std::string& shortcut) {
                auto& hm = tools3000::core::HotkeyManager::instance();
                if (shortcut.empty()) {
                    hm.clearHotkey(hotkeyName);
                    tools3000::core::ConfigManager::instance().set("/hotkeys/" + hotkeyName, "");
                    return;
                }
                if (const auto parsed = tools3000::core::HotkeyDef::fromString(shortcut);
                    parsed && parsed->isSafe()) {
                    hm.rebindHotkey(hotkeyName, *parsed);
                    tools3000::core::ConfigManager::instance().set("/hotkeys/" + hotkeyName, parsed->toString());
                }
            };
            if (params.contains("quickJumpHotkey") && params["quickJumpHotkey"].is_string()) {
                mgr.setQuickJumpHotkey(params["quickJumpHotkey"].get<std::string>());
                mirrorDialogHotkeyToManager("Dialog Quick Jump", mgr.getQuickJumpHotkey());
            }
            if (params.contains("inlineSearchHotkey") && params["inlineSearchHotkey"].is_string()) {
                mgr.setInlineSearchHotkey(params["inlineSearchHotkey"].get<std::string>());
                mirrorDialogHotkeyToManager("Dialog Inline Search", mgr.getInlineSearchHotkey());
            }
            const bool reEnabledRibbon =
                params.contains("ribbonEnabled") && params["ribbonEnabled"].is_boolean() &&
                params["ribbonEnabled"].get<bool>();
            DialogEngine::instance().syncScopedHotkeys();
            DialogEngine::instance().refreshActiveDialogOverlay(reEnabledRibbon);
            return {{"success", true}};
        });

        bridge.registerAsyncHandler("dialog.browseDirectory", [](const json&) -> json {
            const auto selected = browseDirectoryDialogUtf8();
            return selected ? json(*selected) : json(nullptr);
        });

        bridge.registerHandler("dialog.getRecentPaths", [](const json&) -> json {
            return PathMemoryManager::instance().getRecentPaths(20);
        });

        bridge.registerHandler("dialog.clearRecentPaths", [](const json&) -> json {
            PathMemoryManager::instance().clearRecentPaths();
            return {{"success", true}};
        });

        bridge.registerHandler("dialog.getFavorites", [](const json&) -> json {
            return PathMemoryManager::instance().getFavorites();
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
                const std::string proc = params["processName"].get<std::string>();
                const std::string path = params.value("workspacePath", "");
                const bool isFixed = params.value("isFixed", false);
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
            const std::string path = ExplorerTracker::instance().getActiveExplorerPath();
            return {{"path", path}};
        });
    }

    void unregisterIpcHandlers() {
        auto& bridge = tools3000::core::MessageBridge::instance();
        bridge.unregisterHandlersByPrefix("dialog.");
    }

    tools3000::core::SubscriptionId m_themeSubId = 0;
    tools3000::core::SubscriptionId m_dialogHotkeysSubId = 0;
};

extern "C" __declspec(dllexport) tools3000::core::IPlugin* CreatePlugin() {
    static DialogEnhancerPlugin s_instance;
    return &s_instance;
}

extern "C" __declspec(dllexport) std::uint32_t GetPluginAbiVersion() {
    return tools3000::core::CurrentPluginAbiVersion;
}

}  // namespace tools3000::dialog

// 全局保存本 DLL 的 HMODULE，供 SetWinEventHook 与 DialogRibbonOverlay 窗口类注册/注销成对使用
HMODULE g_hDialogEnhancerModule = nullptr;

BOOL APIENTRY DllMain(HMODULE hModule, DWORD ul_reason_for_call, LPVOID) {
    switch (ul_reason_for_call) {
        case DLL_PROCESS_ATTACH:
            DisableThreadLibraryCalls(hModule);
            g_hDialogEnhancerModule = hModule;
            break;
        case DLL_PROCESS_DETACH:
            g_hDialogEnhancerModule = nullptr;
            break;
    }
    return TRUE;
}
