#include "core/plugin/IPlugin.h"
#include "core/logger/Logger.h"
#include "core/hotkey/KeyboardHook.h"
#include "core/ipc/MessageBridge.h"
#include "core/config/ConfigManager.h"
#include "core/hotkey/HotkeyManager.h"
#include "core/events/EventBus.h"
#include "core/events/MainThreadDispatcher.h"
#include "core/utils/WinUtils.h"
#include "gesture/GestureEngine.h"
#include "gesture/MouseHook.h"
#include "gesture/BuiltinCommands.h"
#include "gesture/HotCornerEngine.h"
#include "gesture/RadialMenuOverlay.h"
#include "gesture/GestureTrailOverlay.h"
#include "gesture/GestureInputPolicy.h"
#include "Tools3000Version.h"
#include <algorithm>
#include <array>
#include <charconv>
#include <optional>
#include <string_view>
#include <unordered_set>
#include <windows.h>

namespace tools3000::gesture {

namespace {

std::optional<int> parseCommandIndex(const std::string_view value) noexcept {
    if (value.empty() || (value.size() > 1 && value.front() == '0')) return std::nullopt;
    for (const char ch : value) {
        if (ch < '0' || ch > '9') return std::nullopt;
    }

    int index = -1;
    const auto [end, error] = std::from_chars(value.data(), value.data() + value.size(), index);
    if (error != std::errc{} || end != value.data() + value.size() || index < 0 ||
        index > static_cast<int>(BuiltinCommand::PasteAsPin)) {
        return std::nullopt;
    }
    return index;
}

int hotCornerCommandIndex(const std::string& value) {
    if (value.empty()) return -1;
    if (value == "capture") return static_cast<int>(BuiltinCommand::TakeScreenshot);
    if (value == "search") return static_cast<int>(BuiltinCommand::ToggleSearch);
    const auto index = parseCommandIndex(value);
    return index.value_or(-1);
}

constexpr std::array<std::pair<const char*, HotCorner>, 4> kHotCorners{{
    {"topLeft", HotCorner::TopLeft},
    {"topRight", HotCorner::TopRight},
    {"bottomLeft", HotCorner::BottomLeft},
    {"bottomRight", HotCorner::BottomRight},
}};

std::optional<std::string> parseHotCornerCommand(const nlohmann::json& value) {
    if (value.is_string()) {
        const auto text = value.get<std::string>();
        if (text.empty() || text == "capture" || text == "search") return text;
        if (parseCommandIndex(text)) return text;
        return std::nullopt;
    }

    int index = -1;
    if (value.is_number_integer()) index = value.get<int>();
    else if (value.is_object() && value.contains("commandIndex") &&
             value["commandIndex"].is_number_integer()) {
        index = value["commandIndex"].get<int>();
    } else {
        return std::nullopt;
    }
    if (index == -1) return std::string{};
    if (index < 0 || index > static_cast<int>(BuiltinCommand::PasteAsPin)) {
        return std::nullopt;
    }
    return std::to_string(index);
}

nlohmann::json updateHotCornerSettings(const nlohmann::json& params) {
    using json = nlohmann::json;
    if (!params.is_object() || params.empty()) {
        return {{"success", false}, {"error", "no hot-corner settings supplied"}};
    }

    static const std::unordered_set<std::string> allowedKeys = {
        "enabled", "autoBypassFullscreen", "delay", "triggerDelay", "corners",
        "topLeft", "topRight", "bottomLeft", "bottomRight"
    };
    for (const auto& [key, value] : params.items()) {
        if (!allowedKeys.contains(key)) {
            return {{"success", false}, {"error", "unsupported setting: " + key}};
        }
    }

    auto& engine = HotCornerEngine::instance();
    json desired = {
        {"enabled", engine.isEnabled()},
        {"autoBypassFullscreen", engine.autoBypassFullscreen()},
        {"triggerDelay", engine.triggerDelay()},
        {"topLeft", engine.getCornerAction(HotCorner::TopLeft)},
        {"topRight", engine.getCornerAction(HotCorner::TopRight)},
        {"bottomLeft", engine.getCornerAction(HotCorner::BottomLeft)},
        {"bottomRight", engine.getCornerAction(HotCorner::BottomRight)},
    };

    if (params.contains("enabled")) {
        if (!params["enabled"].is_boolean()) {
            return {{"success", false}, {"error", "enabled must be boolean"}};
        }
        desired["enabled"] = params["enabled"];
    }

    if (params.contains("autoBypassFullscreen")) {
        if (!params["autoBypassFullscreen"].is_boolean()) {
            return {{"success", false}, {"error", "autoBypassFullscreen must be boolean"}};
        }
        desired["autoBypassFullscreen"] = params["autoBypassFullscreen"];
    }

    const char* delayKey = params.contains("delay") ? "delay" :
                           (params.contains("triggerDelay") ? "triggerDelay" : nullptr);
    if (delayKey) {
        if (!params[delayKey].is_number_integer()) {
            return {{"success", false}, {"error", "delay must be an integer"}};
        }
        desired["triggerDelay"] = std::clamp(params[delayKey].get<int>(), 100, 2000);
    }

    if (params.contains("corners")) {
        if (!params["corners"].is_object()) {
            return {{"success", false}, {"error", "corners must be an object"}};
        }
        for (const auto& [key, value] : params["corners"].items()) {
            const auto known = std::ranges::find_if(kHotCorners, [&key](const auto& entry) {
                return key == entry.first;
            });
            if (known == kHotCorners.end()) {
                return {{"success", false}, {"error", "unsupported corner: " + key}};
            }
            const auto command = parseHotCornerCommand(value);
            if (!command) {
                return {{"success", false}, {"error", "invalid command for " + key}};
            }
            desired[key] = *command;
        }
    }

    for (const auto& [key, corner] : kHotCorners) {
        if (!params.contains(key)) continue;
        const auto command = parseHotCornerCommand(params[key]);
        if (!command) {
            return {{"success", false}, {"error", std::string("invalid command for ") + key}};
        }
        desired[key] = *command;
    }

    if (!tools3000::core::ConfigManager::instance().mergePatch(
            {{"gesture", {{"hotCorners", desired}}}}, "/gesture/hotCorners")) {
        return {{"success", false}, {"error", "failed to persist hot-corner settings"}};
    }

    engine.setEnabled(desired["enabled"].get<bool>());
    engine.setAutoBypassFullscreen(desired.value("autoBypassFullscreen", true));
    engine.setTriggerDelay(desired["triggerDelay"].get<int>());
    for (const auto& [key, corner] : kHotCorners) {
        engine.setCornerAction(corner, desired[key].get<std::string>());
    }
    return {{"success", true}, {"settings", std::move(desired)}};
}

tools3000::core::HotkeyDef configuredHotkey(const std::string& name,
                                       const tools3000::core::HotkeyDef& fallback) {
    const auto text = tools3000::core::ConfigManager::instance().get<std::string>(
        "/hotkeys/" + name, fallback.toString());
    if (text.empty()) return {};
    return tools3000::core::HotkeyDef::fromString(text).value_or(fallback);
}

}  // namespace

class GesturePlugin : public tools3000::core::IPlugin {
public:
    const char* getName() const override { return "Gesture"; }
    const char* getVersion() const override { return tools3000::version::String; }

    bool initialize() override {
        LOG_INFO("GesturePlugin: 初始化手势引擎");

        // 注册内置命令处理器
        using tools3000::gesture::BuiltinCommand;
        auto& dispatcher = tools3000::gesture::BuiltinCommandDispatcher::instance();
        dispatcher.registerHandler(BuiltinCommand::PauseGestures, []() {
            tools3000::core::MainThreadDispatcher::instance().post([]() {
                auto& engine = tools3000::gesture::GestureEngine::instance();
                engine.setPaused(!engine.isPaused());
            });
        });
        dispatcher.registerHandler(BuiltinCommand::TakeScreenshot, []() {
            tools3000::core::MainThreadDispatcher::instance().post([]() {
                tools3000::core::EventBus::instance().publish(tools3000::core::ActionTriggerScreenshotEvent{});
            });
        });
        dispatcher.registerHandler(BuiltinCommand::StartRecording, []() {
            tools3000::core::MainThreadDispatcher::instance().post([]() {
                tools3000::core::EventBus::instance().publish(tools3000::core::ActionToggleRecordingEvent{});
            });
        });
        dispatcher.registerHandler(BuiltinCommand::ToggleSearch, []() {
            tools3000::core::MainThreadDispatcher::instance().post([]() {
                tools3000::core::MessageBridge::instance().handleMessage(R"({"method":"search.toggle"})");
            });
        });
        dispatcher.registerHandler(BuiltinCommand::PasteAsPin, []() {
            tools3000::core::MainThreadDispatcher::instance().post([]() {
                tools3000::core::MessageBridge::instance().handleMessage(R"({"method":"capture.pasteAsPin"})");
            });
        });
        dispatcher.registerHandler(BuiltinCommand::ShowRadialMenu, []() {
            tools3000::core::MainThreadDispatcher::instance().post([]() {
                LOG_INFO("GesturePlugin: 内置命令触发轮盘菜单");
                POINT pt;
                GetCursorPos(&pt);
                tools3000::gesture::RadialMenuOverlay::instance().show(pt);
            });
        });

        // 状态变化回调
        auto& gestureEngine = tools3000::gesture::GestureEngine::instance();
        gestureEngine.setPauseChangedCallback([](bool paused) -> bool {
            auto& config = tools3000::core::ConfigManager::instance();
            const bool enabled = !paused;
            const bool alreadySaved =
                config.get<bool>("/gesture/paused", !paused) == paused &&
                config.get<bool>("/gesture/enabled", paused) == enabled &&
                config.get<bool>("/plugins/gesture/enabled", paused) == enabled;
            if (!alreadySaved && !config.mergePatch({
                    {"gesture", {{"paused", paused}, {"enabled", enabled}}},
                    {"plugins", {{"gesture", {{"enabled", enabled}}}}}
                }, "/gesture")) {
                return false;
            }
            tools3000::core::ConfigManager::instance().set<bool>("/plugins/gesture/enabled", enabled);
            tools3000::core::MessageBridge::instance().pushEvent("gesture.stateChanged", {
                {"paused", paused},
                {"enabled", enabled}
            });
            tools3000::core::MessageBridge::instance().pushEvent("gesture:stateChanged", {
                {"paused", paused},
                {"enabled", enabled}
            });
            tools3000::core::MessageBridge::instance().pushEvent("plugins:changed", {
                {"id", "gesture"},
                {"enabled", enabled}
            });
            return true;
        });

        // 注册 IPC 处理器
        
        auto& mb = tools3000::core::MessageBridge::instance();
        auto& bus = tools3000::core::EventBus::instance();

        m_pauseSubscription = bus.subscribe<tools3000::core::ActionToggleGesturePauseEvent>([](const tools3000::core::ActionToggleGesturePauseEvent&) {
            auto& engine = tools3000::gesture::GestureEngine::instance();
            engine.setPaused(!engine.isPaused());
        });
        m_cancelSubscription = bus.subscribe<tools3000::core::CancelTransientUiEvent>([](const tools3000::core::CancelTransientUiEvent&) {
            tools3000::gesture::GestureEngine::instance().cancelActiveGesture();
            tools3000::gesture::RadialMenuOverlay::instance().hide();
            tools3000::core::WinUtils::emergencyFlushInputState();
        });
        m_sessionSubscription = bus.subscribe<tools3000::core::SystemSessionChangedEvent>([](const tools3000::core::SystemSessionChangedEvent&) {
            tools3000::gesture::GestureEngine::instance().cancelActiveGesture();
            tools3000::gesture::MouseHook::instance().resetTriggerState();
            tools3000::gesture::RadialMenuOverlay::instance().hide();
            tools3000::core::WinUtils::emergencyFlushInputState();
        });
        m_powerSubscription = bus.subscribe<tools3000::core::SystemPowerChangedEvent>([](const tools3000::core::SystemPowerChangedEvent&) {
            tools3000::gesture::GestureEngine::instance().cancelActiveGesture();
            tools3000::gesture::MouseHook::instance().resetTriggerState();
            tools3000::gesture::RadialMenuOverlay::instance().hide();
            tools3000::core::WinUtils::emergencyFlushInputState();
        });
        m_themeSubscription = bus.subscribe<tools3000::core::ThemeChangedEvent>([](const tools3000::core::ThemeChangedEvent&) {
            tools3000::gesture::GestureTrailOverlay::instance().reloadThemeColors();
        });
        
        mb.registerHandler("gesture.togglePause", [](const nlohmann::json&) -> nlohmann::json {
            auto& engine = tools3000::gesture::GestureEngine::instance();
            const bool success = engine.setPaused(!engine.isPaused());
            return {{"success", success}, {"paused", engine.isPaused()},
                    {"enabled", !engine.isPaused()}};
        });

        auto& hotkeys = tools3000::core::HotkeyManager::instance();
        hotkeys.registerHotkey("Pause Gestures", configuredHotkey("Pause Gestures", {tools3000::core::ModKey::Ctrl | tools3000::core::ModKey::Alt | tools3000::core::ModKey::Shift, 'W'}), []() {
            auto& engine = tools3000::gesture::GestureEngine::instance();
            engine.setPaused(!engine.isPaused());
        });

        mb.registerHandler("gesture.getProfiles", [](const nlohmann::json&) -> nlohmann::json {
            auto& engine = tools3000::gesture::GestureEngine::instance();
            nlohmann::json result = nlohmann::json::array();
            for (const auto& profile : engine.getProfiles()) result.push_back(profile.toJson());
            return result;
        });

        mb.registerHandler("gesture.getState", [](const nlohmann::json&) -> nlohmann::json {
            auto& engine = tools3000::gesture::GestureEngine::instance();
            auto& config = tools3000::core::ConfigManager::instance();
            return {
                {"paused", engine.isPaused()},
                {"enabled", !engine.isPaused()},
                {"triggerButton", engine.triggerButton()},
                {"trailVisible", engine.trailVisible()},
                {"autoBypassFullscreen", engine.autoBypassFullscreen()},
                {"enableScribbleCancel", config.get<bool>("/gesture/enableScribbleCancel", true)},
                {"enableInFlightCompass", config.get<bool>("/gesture/enableInFlightCompass", true)},
                {"targetMode", engine.targetMode()},
                {"initialTimeoutMs", engine.initialTimeoutMs()},
                {"minSegmentDistance", engine.minSegmentDistance()},
                {"trailColorMode", config.get<std::string>("/gesture/trailColorMode", "auto")},
                {"trailColor", config.get<std::string>("/gesture/trailColor", "#3B82F6")},
                {"trailWidth", config.get<float>("/gesture/trailWidth", 2.5f)},
                {"trailOutlineWidth", config.get<float>("/gesture/trailOutlineWidth", 1.5f)},
                {"elevated", tools3000::core::WinUtils::isCurrentProcessElevated()},
                {"runAsAdmin", config.get<bool>("/general/runAsAdmin", true)}
            };
        });

        mb.registerHandler("gesture.updateSettings", [](const nlohmann::json& params) -> nlohmann::json {
            auto& engine = tools3000::gesture::GestureEngine::instance();
            auto& config = tools3000::core::ConfigManager::instance();
            if (!params.is_object() || params.empty()) {
                return {{"success", false}, {"error", "no gesture settings supplied"}};
            }
            static const std::unordered_set<std::string> allowed = {
                "enabled", "paused", "triggerButton", "trailVisible", "autoBypassFullscreen",
                "targetMode", "initialTimeoutMs", "minSegmentDistance",
                "trailColorMode", "trailColor", "trailWidth", "trailOutlineWidth",
                "enableScribbleCancel", "enableInFlightCompass"
            };
            for (const auto& [key, value] : params.items()) {
                if (!allowed.contains(key)) {
                    return {{"success", false}, {"error", "unsupported setting: " + key}};
                }
            }
            if ((params.contains("enabled") && !params["enabled"].is_boolean()) ||
                (params.contains("paused") && !params["paused"].is_boolean()) ||
                (params.contains("trailVisible") && !params["trailVisible"].is_boolean()) ||
                (params.contains("autoBypassFullscreen") && !params["autoBypassFullscreen"].is_boolean()) ||
                (params.contains("enableScribbleCancel") && !params["enableScribbleCancel"].is_boolean()) ||
                (params.contains("enableInFlightCompass") && !params["enableInFlightCompass"].is_boolean()) ||
                (params.contains("targetMode") && !params["targetMode"].is_string()) ||
                (params.contains("initialTimeoutMs") && !params["initialTimeoutMs"].is_number_integer()) ||
                (params.contains("minSegmentDistance") && !params["minSegmentDistance"].is_number_integer()) ||
                (params.contains("trailColorMode") && !params["trailColorMode"].is_string()) ||
                (params.contains("trailColor") && !params["trailColor"].is_string()) ||
                (params.contains("trailWidth") && !params["trailWidth"].is_number()) ||
                (params.contains("trailOutlineWidth") && !params["trailOutlineWidth"].is_number())) {
                return {{"success", false}, {"error", "setting has invalid type"}};
            }
            if (params.contains("enabled") && params.contains("paused") &&
                params["enabled"].get<bool>() == params["paused"].get<bool>()) {
                return {{"success", false}, {"error", "enabled and paused conflict"}};
            }

            std::string trigger = engine.triggerButton();
            if (params.contains("triggerButton")) {
                if (!params["triggerButton"].is_string()) {
                    return {{"success", false}, {"error", "triggerButton must be a string"}};
                }
                trigger = params["triggerButton"].get<std::string>();
                if (trigger != "right" && trigger != "middle" && trigger != "both" &&
                    trigger != "all" && trigger != "x1" && trigger != "x2") {
                    return {{"success", false}, {"error", "invalid triggerButton"}};
                }
            }

            bool paused = engine.isPaused();
            if (params.contains("enabled")) paused = !params["enabled"].get<bool>();
            if (params.contains("paused")) paused = params["paused"].get<bool>();
            const bool trailVisible = params.value("trailVisible", engine.trailVisible());
            const bool autoBypassFullscreen = params.value("autoBypassFullscreen", engine.autoBypassFullscreen());
            std::string targetMode = params.value("targetMode", engine.targetMode());
            if (targetMode != "underPointer" && targetMode != "foreground") {
                return {{"success", false}, {"error", "invalid targetMode"}};
            }

            const int initialTimeoutMs = params.value("initialTimeoutMs", engine.initialTimeoutMs());
            const int minSegmentDistance = params.value("minSegmentDistance", engine.minSegmentDistance());

            std::string trailColorMode = params.value("trailColorMode", config.get<std::string>("/gesture/trailColorMode", "auto"));
            std::string trailColor = params.value("trailColor", config.get<std::string>("/gesture/trailColor", "#3B82F6"));
            float trailWidth = params.value("trailWidth", config.get<float>("/gesture/trailWidth", 2.5f));
            float trailOutlineWidth = clampTrailOutlineWidth(
                params.value("trailOutlineWidth", config.get<float>("/gesture/trailOutlineWidth", 1.5f)));
            bool enableScribbleCancel = params.value("enableScribbleCancel", config.get<bool>("/gesture/enableScribbleCancel", true));
            bool enableInFlightCompass = params.value("enableInFlightCompass", config.get<bool>("/gesture/enableInFlightCompass", true));

            nlohmann::json patch = {
                {"paused", paused}, {"enabled", !paused},
                {"triggerButton", trigger}, {"trailVisible", trailVisible},
                {"autoBypassFullscreen", autoBypassFullscreen},
                {"enableScribbleCancel", enableScribbleCancel},
                {"enableInFlightCompass", enableInFlightCompass},
                {"targetMode", targetMode},
                {"initialTimeoutMs", initialTimeoutMs},
                {"minSegmentDistance", minSegmentDistance},
                {"trailColorMode", trailColorMode},
                {"trailColor", trailColor},
                {"trailWidth", trailWidth},
                {"trailOutlineWidth", trailOutlineWidth}
            };
            if (!config.mergePatch({{"gesture", patch}}, "/gesture")) {
                return {{"success", false}, {"error", "failed to persist gesture settings"}};
            }
            engine.setTriggerButton(trigger);
            engine.setTrailVisible(trailVisible);
            engine.setAutoBypassFullscreen(autoBypassFullscreen);
            engine.setTargetMode(targetMode);
            engine.setInitialTimeoutMs(initialTimeoutMs);
            engine.setMinSegmentDistance(minSegmentDistance);
            tools3000::gesture::RecognizerConfig recConfig;
            recConfig.minSegmentDistance = 14;
            recConfig.samplingInterval = 2;
            recConfig.angleToleranceDeg = 22.5;
            recConfig.enableScribbleCancel = enableScribbleCancel;
            engine.setRecognizerConfig(recConfig);
            tools3000::gesture::GestureTrailOverlay::instance().reloadThemeColors();

            const bool pauseApplied = engine.setPaused(paused);
            return {
                {"success", pauseApplied},
                {"paused", engine.isPaused()},
                {"enabled", !engine.isPaused()},
                {"triggerButton", engine.triggerButton()},
                {"trailVisible", engine.trailVisible()},
                {"autoBypassFullscreen", engine.autoBypassFullscreen()},
                {"targetMode", engine.targetMode()},
                {"initialTimeoutMs", engine.initialTimeoutMs()},
                {"minSegmentDistance", engine.minSegmentDistance()},
                {"trailColorMode", trailColorMode},
                {"trailColor", trailColor},
                {"trailWidth", trailWidth},
                {"trailOutlineWidth", trailOutlineWidth}
            };
        });

        mb.registerHandler("gesture.updateProfile", [](const nlohmann::json& params) -> nlohmann::json {
            if (!params.is_object() || !params.contains("name") || !params["name"].is_string() ||
                params["name"].get<std::string>().empty() ||
                !params.contains("mappings") || !params["mappings"].is_array()) {
                return {{"success", false}, {"error", "invalid profile"}};
            }
            auto profile = tools3000::gesture::GestureProfile::fromJson(params);
            auto& engine = tools3000::gesture::GestureEngine::instance();
            const auto previous = engine.getProfile(profile.name());
            engine.setProfile(profile.name(), profile);
            if (!engine.saveToConfig()) {
                if (previous) engine.setProfile(previous->name(), *previous);
                else engine.removeProfile(profile.name());
                return {{"success", false}, {"error", "failed to persist profile"}};
            }
            return {{"success", true}};
        });

        mb.registerHandler("gesture.setTriggerState", [](const nlohmann::json& params) -> nlohmann::json {
            std::string profName = params.value("profile", "default");
            std::string trigger = params.value("trigger", "");
            std::string stateStr = params.value("state", "default");
            if (trigger.empty()) return {{"success", false}, {"error", "trigger is required"}};

            auto& engine = tools3000::gesture::GestureEngine::instance();
            auto profile = engine.getProfile(profName);
            if (!profile) profile = tools3000::gesture::GestureProfile(profName);

            profile->setTriggerState(trigger, tools3000::gesture::triggerStateFromString(stateStr));
            engine.setProfile(profName, *profile);
            bool ok = engine.saveToConfig();
            return {{"success", ok}};
        });

        mb.registerHandler("gesture.setTriggerBatch", [](const nlohmann::json& params) -> nlohmann::json {
            std::string profName = params.value("profile", "default");
            std::string stateStr = params.value("state", "default");

            auto& engine = tools3000::gesture::GestureEngine::instance();
            auto profile = engine.getProfile(profName);
            if (!profile) profile = tools3000::gesture::GestureProfile(profName);

            profile->setAllTriggerStates(tools3000::gesture::triggerStateFromString(stateStr));
            engine.setProfile(profName, *profile);
            bool ok = engine.saveToConfig();
            return {{"success", ok}};
        });

        mb.registerHandler("gesture.reorderMappings", [](const nlohmann::json& params) -> nlohmann::json {
            std::string profName = params.value("profile", "default");
            auto& engine = tools3000::gesture::GestureEngine::instance();
            auto profile = engine.getProfile(profName);
            if (!profile) return {{"success", false}, {"error", "profile not found"}};

            if (params.contains("fromIndex") && params.contains("toIndex")) {
                size_t fromIdx = params["fromIndex"].get<size_t>();
                size_t toIdx = params["toIndex"].get<size_t>();
                profile->moveMapping(fromIdx, toIdx);
            } else if (params.contains("orderedCodes") && params["orderedCodes"].is_array()) {
                std::vector<std::string> codes = params["orderedCodes"].get<std::vector<std::string>>();
                profile->reorderMappings(codes);
            }

            engine.setProfile(profName, *profile);
            bool ok = engine.saveToConfig();
            return {{"success", ok}};
        });

        mb.registerHandler("gesture.setPaused", [](const nlohmann::json& params) -> nlohmann::json {
            bool paused = params.value("paused", false);
            const bool success = tools3000::gesture::GestureEngine::instance().setPaused(paused);
            return {
                {"success", success},
                {"paused", tools3000::gesture::GestureEngine::instance().isPaused()},
                {"enabled", !tools3000::gesture::GestureEngine::instance().isPaused()}
            };
        });

        mb.registerHandler("gesture.setRecordingMode", [](const nlohmann::json& params) -> nlohmann::json {
            bool recording = params.value("recording", false);
            tools3000::gesture::GestureEngine::instance().setRecordingMode(recording);
            return {
                {"success", true},
                {"recording", tools3000::gesture::GestureEngine::instance().isRecordingMode()}
            };
        });

        mb.registerHandler("gesture.getScopeRules", [](const nlohmann::json&) -> nlohmann::json {
            return tools3000::gesture::GestureEngine::instance().scopeRules().toJson();
        });

        mb.registerHandler("gesture.updateScopeRules", [](const nlohmann::json& params) -> nlohmann::json {
            const nlohmann::json& rules = (params.is_object() && params.contains("rules")) ? params["rules"] : params;
            if (!rules.is_array()) {
                return {{"success", false}, {"error", "rules must be an array"}};
            }
            auto& engine = tools3000::gesture::GestureEngine::instance();
            const auto previous = engine.scopeRules().toJson();
            engine.scopeRules().loadFromJson(rules);
            if (!engine.saveToConfig()) {
                engine.scopeRules().loadFromJson(previous);
                return {{"success", false}, {"error", "failed to persist scope rules"}};
            }
            return {{"success", true}};
        });

        // 注册 HotCorner 相关 IPC
        mb.registerHandler("gesture.updateHotCorners", [](const nlohmann::json& params) -> nlohmann::json {
            return updateHotCornerSettings(params);
        });

        // hotcorner.getSettings —— 返回四个角的动作和启用状态
        mb.registerHandler("hotcorner.getSettings", [](const nlohmann::json&) -> nlohmann::json {
            auto& hce = tools3000::gesture::HotCornerEngine::instance();
            auto& config = tools3000::core::ConfigManager::instance();
            LOG_DEBUG("IPC: hotcorner.getSettings 查询触发角配置");
            return {
                {"enabled", hce.isEnabled()},
                {"autoBypassFullscreen", hce.autoBypassFullscreen()},
                {"delay", config.get<int>("/gesture/hotCorners/triggerDelay", 300)},
                {"corners", {
                    {"topLeft", {{"commandIndex", hotCornerCommandIndex(hce.getCornerAction(tools3000::gesture::HotCorner::TopLeft))}}},
                    {"topRight", {{"commandIndex", hotCornerCommandIndex(hce.getCornerAction(tools3000::gesture::HotCorner::TopRight))}}},
                    {"bottomLeft", {{"commandIndex", hotCornerCommandIndex(hce.getCornerAction(tools3000::gesture::HotCorner::BottomLeft))}}},
                    {"bottomRight", {{"commandIndex", hotCornerCommandIndex(hce.getCornerAction(tools3000::gesture::HotCorner::BottomRight))}}}
                }}
            };
        });

        // hotcorner.updateSettings —— 更新角落动作/延迟/启用状态
        mb.registerHandler("hotcorner.updateSettings", [](const nlohmann::json& params) -> nlohmann::json {
            LOG_INFO("IPC: hotcorner.updateSettings 更新触发角配置");
            return updateHotCornerSettings(params);
        });

        // 注册 RadialMenu 弹出接口
        mb.registerHandler("gesture.showRadialMenu", [](const nlohmann::json&) -> nlohmann::json {
            POINT pt;
            GetCursorPos(&pt);
            tools3000::core::MainThreadDispatcher::instance().post([pt]() {
                tools3000::gesture::RadialMenuOverlay::instance().show(pt);
            });
            return {{"success", true}};
        });

        // radialmenu.getItems —— 返回菜单项列表
        mb.registerHandler("radialmenu.getItems", [](const nlohmann::json&) -> nlohmann::json {
            LOG_DEBUG("IPC: radialmenu.getItems 查询轮盘菜单项");
            auto& config = tools3000::core::ConfigManager::instance();
            auto items = config.get<nlohmann::json>("/gesture/radialMenu/items", nlohmann::json::array());
            return {{"items", items}};
        });

        // radialmenu.updateItems —— 更新菜单项
        mb.registerHandler("radialmenu.updateItems", [](const nlohmann::json& params) -> nlohmann::json {
            LOG_INFO("IPC: radialmenu.updateItems 更新轮盘菜单项");
            auto& config = tools3000::core::ConfigManager::instance();

            if (!params.contains("items") || !params["items"].is_array()) {
                LOG_WARN("radialmenu.updateItems: 缺少 items 数组参数");
                return {{"success", false}, {"error", "missing items array"}};
            }

            const auto& itemsJson = params["items"];
            if (itemsJson.size() > 16) {
                return {{"success", false}, {"error", "too many radial-menu items"}};
            }
            std::vector<tools3000::gesture::RadialMenuItem> items;
            items.reserve(itemsJson.size());
            for (const auto& ij : itemsJson) {
                if (!ij.is_object() || !ij.contains("label") || !ij["label"].is_string() ||
                    !ij.contains("command") || !ij["command"].is_string()) {
                    return {{"success", false}, {"error", "invalid radial-menu item"}};
                }
                const auto label = ij["label"].get<std::string>();
                const auto command = ij["command"].get<std::string>();
                if (label.empty() || label.size() > 80 || command.size() > 256) {
                    return {{"success", false}, {"error", "invalid radial-menu item length"}};
                }
                items.push_back({
                    label,
                    command
                });
            }

            // 持久化到配置
            if (!config.set("/gesture/radialMenu/items", itemsJson)) {
                return {{"success", false}, {"error", "failed to persist radial-menu items"}};
            }
            tools3000::gesture::RadialMenuOverlay::instance().setItems(items);
            LOG_INFO("radialmenu.updateItems: 已更新 {} 个菜单项", items.size());
            return {{"success", true}, {"count", items.size()}};
        });

        gestureEngine.loadFromConfig();
        
        // 加载 HotCorner 配置并启动
        auto& config = tools3000::core::ConfigManager::instance();
        auto& hce = tools3000::gesture::HotCornerEngine::instance();
        hce.setCornerAction(tools3000::gesture::HotCorner::TopLeft, config.get<std::string>("/gesture/hotCorners/topLeft", ""));
        hce.setCornerAction(tools3000::gesture::HotCorner::TopRight, config.get<std::string>("/gesture/hotCorners/topRight", "capture")); // 默认右上角截图
        hce.setCornerAction(tools3000::gesture::HotCorner::BottomLeft, config.get<std::string>("/gesture/hotCorners/bottomLeft", ""));
        hce.setCornerAction(tools3000::gesture::HotCorner::BottomRight, config.get<std::string>("/gesture/hotCorners/bottomRight", "search")); // 默认右下角搜索
        hce.setEnabled(config.get<bool>("/gesture/hotCorners/enabled", false));
        hce.setAutoBypassFullscreen(config.get<bool>("/gesture/hotCorners/autoBypassFullscreen", true));
        hce.setTriggerDelay(std::clamp(config.get<int>("/gesture/hotCorners/triggerDelay", 300), 100, 2000));

        // 初始化 RadialMenu — 优先从配置加载，无配置时使用默认项
        auto savedRadialItems = config.get<nlohmann::json>("/gesture/radialMenu/items", nlohmann::json());
        if (savedRadialItems.is_array() && !savedRadialItems.empty()) {
            std::vector<tools3000::gesture::RadialMenuItem> rItems;
            rItems.reserve(savedRadialItems.size());
            for (const auto& ij : savedRadialItems) {
                rItems.push_back({ij.value("label", ""), ij.value("command", "")});
            }
            tools3000::gesture::RadialMenuOverlay::instance().setItems(rItems);
            LOG_INFO("GesturePlugin: 从配置加载 {} 个轮盘菜单项", rItems.size());
        } else {
            std::vector<tools3000::gesture::RadialMenuItem> rItems = {
                {"截图 (Top)", "capture"},
                {"搜索 (Right)", "search"},
                {"锁定 (Bottom)", "lock"},
                {"贴图 (Left)", "pin"}
            };
            tools3000::gesture::RadialMenuOverlay::instance().setItems(rItems);
            LOG_INFO("GesturePlugin: 使用默认轮盘菜单项, 数量={}", rItems.size());
        }

        if (!gestureEngine.start()) {
            LOG_ERROR("GesturePlugin: 手势引擎启动失败");
            return false;
        }
        hce.start();
        return true;
    }

    void shutdown() override {
        LOG_INFO("GesturePlugin: 卸载手势引擎");
        auto& bridge = tools3000::core::MessageBridge::instance();
        bridge.unregisterHandlersByPrefix("gesture.");
        bridge.unregisterHandlersByPrefix("hotcorner.");
        bridge.unregisterHandlersByPrefix("radialmenu.");
        tools3000::core::HotkeyManager::instance().unregisterHotkey("Pause Gestures");
        tools3000::core::EventBus::instance().unsubscribeAndWait(m_pauseSubscription);
        m_pauseSubscription = 0;
        tools3000::core::EventBus::instance().unsubscribeAndWait(m_cancelSubscription);
        m_cancelSubscription = 0;
        tools3000::core::EventBus::instance().unsubscribeAndWait(m_sessionSubscription);
        m_sessionSubscription = 0;
        tools3000::core::EventBus::instance().unsubscribeAndWait(m_powerSubscription);
        m_powerSubscription = 0;
        tools3000::core::EventBus::instance().unsubscribeAndWait(m_themeSubscription);
        m_themeSubscription = 0;

        auto& gestureEngine = tools3000::gesture::GestureEngine::instance();
        gestureEngine.saveToConfig();
        gestureEngine.stop();
        gestureEngine.setPauseChangedCallback(nullptr);
        tools3000::gesture::HotCornerEngine::instance().stop();
        tools3000::gesture::BuiltinCommandDispatcher::instance().clearHandlers();
    }

private:
    tools3000::core::SubscriptionId m_pauseSubscription = 0;
    tools3000::core::SubscriptionId m_cancelSubscription = 0;
    tools3000::core::SubscriptionId m_sessionSubscription = 0;
    tools3000::core::SubscriptionId m_powerSubscription = 0;
    tools3000::core::SubscriptionId m_themeSubscription = 0;
};

} // namespace tools3000::gesture

PLUGIN_API tools3000::core::IPlugin* CreatePlugin() {
    static tools3000::gesture::GesturePlugin instance;
    return &instance;
}

PLUGIN_API std::uint32_t GetPluginAbiVersion() {
    return tools3000::core::CurrentPluginAbiVersion;
}
