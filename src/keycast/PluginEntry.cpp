#include "core/plugin/IPlugin.h"
#include "Tools3000Version.h"
#include "core/logger/Logger.h"
#include "core/hotkey/KeyboardHook.h"
#include "core/ipc/MessageBridge.h"
#include "core/config/ConfigManager.h"
#include "core/events/EventBus.h"
#include "core/events/MainThreadDispatcher.h"
#include "core/utils/WinUtils.h"
#include "KeycastOverlay.h"
#include <windows.h>
#include <atomic>
#include <chrono>
#include <future>

namespace tools3000::keycast {

namespace {

std::atomic<bool> g_pluginShuttingDown{false};

bool isPrintableCharacterToken(const std::string& tok) noexcept {
    if (tok.empty()) return false;
    if (tok.size() == 1) {
        const unsigned char ch = static_cast<unsigned char>(tok[0]);
        return ch >= 0x21 && ch <= 0x7E;
    }
    return tok.rfind("OEM", 0) == 0;
}

std::string decodeAltGrCharacterOnUiThread(DWORD vkCode, DWORD scanCode, bool shiftDown) {
    if (vkCode == 0) return {};
    BYTE state[256] = {};
    state[VK_CONTROL] = 0x80;
    state[VK_LCONTROL] = 0x80;
    state[VK_MENU] = 0x80;
    state[VK_RMENU] = 0x80;
    if (shiftDown) {
        state[VK_SHIFT] = 0x80;
        state[VK_LSHIFT] = 0x80;
    }
    HWND fg = GetForegroundWindow();
    DWORD fgTid = fg ? GetWindowThreadProcessId(fg, nullptr) : 0;
    HKL hkl = GetKeyboardLayout(fgTid);
    wchar_t wbuf[8] = {};
    // Flag 0x04 avoids mutating kernel dead-key state on Windows 10 1607+.
    int rc = ToUnicodeEx(vkCode, scanCode, state, wbuf, 4, 0x00000004, hkl);
    if (rc <= 0) {
        return {};
    }
    if (rc == 1 && wbuf[0] < 0x20) {
        return {};
    }
    std::wstring ws(wbuf, static_cast<size_t>(rc));
    return tools3000::core::WinUtils::wstringToUtf8(ws);
}

} // namespace

class KeycastPlugin : public tools3000::core::IPlugin {
public:
    const char* getName() const override { return "Keycast"; }
    const char* getVersion() const override { return tools3000::version::String; }

    bool initialize() override {
        LOG_INFO("Keycast Plugin initialize");
        
        g_pluginShuttingDown.store(false, std::memory_order_release);

        // Initialize Overlay UI
        if (!KeycastOverlay::instance().init()) {
            LOG_ERROR("Failed to init KeycastOverlay");
            return false;
        }

        // 复用 core::KeyboardHook 单一底层钩子产出的 AltGr 状态机与扫描码，杜绝主线程重复挂载第二条 WH_KEYBOARD_LL
        tools3000::core::KeyboardHook::instance().setKeycastKeyInfoCallback([](const tools3000::core::KeycastKeyInfo& info) {
            if (g_pluginShuttingDown.load(std::memory_order_acquire)) {
                return;
            }
            const auto containsToken = [&info](const char* target) noexcept {
                for (const auto& tok : info.tokens) {
                    if (tok == target) return true;
                }
                return false;
            };
            const bool hasShift = containsToken("Shift");

            if (info.isPureAltGr) {
                const std::string lastToken = info.tokens.empty() ? std::string{} : info.tokens.back();
                const bool isModOnly = lastToken.empty() ||
                                       lastToken == "Ctrl" || lastToken == "Alt" ||
                                       lastToken == "Shift" || lastToken == "Win";
                if (isModOnly) {
                    return;
                }
                if (isPrintableCharacterToken(lastToken)) {
                    const DWORD vk = info.vkCode;
                    const DWORD sc = info.rawScanCode;
                    const bool shiftDown = hasShift;
                    auto& disp = tools3000::core::MainThreadDispatcher::instance();
                    if (disp.isInitialized()) {
                        disp.postDeferred([vk, sc, shiftDown]() {
                            if (g_pluginShuttingDown.load(std::memory_order_acquire)) return;
                            const KeycastSettings cur = KeycastOverlay::instance().getSettings();
                            if (cur.filterMode != "all_keys") {
                                return;
                            }
                            const std::string decoded = decodeAltGrCharacterOnUiThread(vk, sc, shiftDown);
                            if (!decoded.empty()) {
                                KeycastOverlay::instance().pushKey(std::vector<std::string>{decoded}, decoded);
                            }
                        });
                    }
                    return;
                }
            }

            auto& disp = tools3000::core::MainThreadDispatcher::instance();
            if (disp.isInitialized()) {
                disp.postDeferred([info]() {
                    if (g_pluginShuttingDown.load(std::memory_order_acquire)) return;
                    KeycastOverlay::instance().pushKey(info);
                });
            } else {
                KeycastOverlay::instance().pushKey(info);
            }
        });

        // 订阅全局主题与主题色变更事件
        m_themeSubscription = tools3000::core::EventBus::instance().subscribe<tools3000::core::ThemeChangedEvent>([](const tools3000::core::ThemeChangedEvent&) {
            KeycastOverlay::instance().onThemeChanged();
        });

        auto& mb = tools3000::core::MessageBridge::instance();
        mb.registerHandler("keycast.getSettings", [](const nlohmann::json&) -> nlohmann::json {
            auto s = KeycastOverlay::instance().getSettings();
            return {
                {"enabled", s.enabled},
                {"autoBypassFullscreen", s.autoBypassFullscreen},
                {"filterMode", s.filterMode},
                {"includeFunctionKeys", s.includeFunctionKeys},
                {"position", s.position},
                {"hasCustomPosition", s.hasCustomPosition},
                {"customX", s.customX},
                {"customY", s.customY},
                {"mergeRecentKeys", s.mergeRecentKeys},
                {"mergeTimeoutMs", s.mergeTimeoutMs},
                {"onlyShortcuts", s.filterMode == "smart_shortcuts"},
                {"displayDurationMs", s.displayDurationMs},
                {"fontSize", s.fontSize},
                {"opacity", s.opacity},
                {"textColor", s.textColor},
                {"backgroundColor", s.backgroundColor},
                {"modifierKeycapColor", s.modifierKeycapColor},
                {"modifierKeycapOpacity", s.modifierKeycapOpacity},
                {"modifierTextColor", s.modifierTextColor},
                {"firstKeyAnim", s.firstKeyAnim},
                {"subsequentKeyAnim", s.subsequentKeyAnim},
                {"rowCascadeAnim", s.rowCascadeAnim},
                {"exitDriftAnim", s.exitDriftAnim}
            };
        });

        mb.registerHandler("keycast.updateSettings", [](const nlohmann::json& params) -> nlohmann::json {
            if (!params.is_object() || params.empty()) {
                return {{"success", false}, {"error", "no settings supplied"}};
            }
            auto& overlay = KeycastOverlay::instance();
            auto s = overlay.getSettings();

            if (params.contains("enabled") && params["enabled"].is_boolean()) {
                const bool enabled = params["enabled"].get<bool>();
                s.enabled = enabled;
                tools3000::core::ConfigManager::instance().set<bool>("/keycast/enabled", enabled);
                tools3000::core::ConfigManager::instance().set<bool>("/general/keycastEnabled", enabled);
                tools3000::core::ConfigManager::instance().set<bool>("/plugins/keycast/enabled", enabled);
                tools3000::core::MessageBridge::instance().pushEvent("keycast:configChanged", {{"enabled", enabled}});
                tools3000::core::MessageBridge::instance().pushEvent("plugins:changed", {{"id", "keycast"}, {"enabled", enabled}});
            }
            if (params.contains("autoBypassFullscreen") && params["autoBypassFullscreen"].is_boolean()) {
                s.autoBypassFullscreen = params["autoBypassFullscreen"].get<bool>();
            }
            if (params.contains("filterMode") && params["filterMode"].is_string()) {
                s.filterMode = params["filterMode"].get<std::string>();
            }
            if (params.contains("includeFunctionKeys") && params["includeFunctionKeys"].is_boolean()) {
                s.includeFunctionKeys = params["includeFunctionKeys"].get<bool>();
            }
            if (params.contains("position") && params["position"].is_string()) {
                s.position = params["position"].get<std::string>();
                // 用户显式切换预设方位时自动复位自定义拖拽偏移
                if (!params.contains("hasCustomPosition")) {
                    s.hasCustomPosition = false;
                }
            }
            if (params.contains("hasCustomPosition") && params["hasCustomPosition"].is_boolean()) {
                s.hasCustomPosition = params["hasCustomPosition"].get<bool>();
            }
            if (params.contains("customX") && params["customX"].is_number_integer()) {
                s.customX = params["customX"].get<int>();
            }
            if (params.contains("customY") && params["customY"].is_number_integer()) {
                s.customY = params["customY"].get<int>();
            }
            if (params.contains("mergeRecentKeys") && params["mergeRecentKeys"].is_boolean()) {
                s.mergeRecentKeys = params["mergeRecentKeys"].get<bool>();
            }
            if (params.contains("mergeTimeoutMs") && params["mergeTimeoutMs"].is_number_integer()) {
                s.mergeTimeoutMs = params["mergeTimeoutMs"].get<int>();
            }
            if (params.contains("onlyShortcuts") && params["onlyShortcuts"].is_boolean()) {
                bool only = params["onlyShortcuts"].get<bool>();
                s.filterMode = only ? "smart_shortcuts" : "all_keys";
            }
            if (params.contains("displayDurationMs") && params["displayDurationMs"].is_number_integer()) {
                s.displayDurationMs = params["displayDurationMs"].get<int>();
            }
            if (params.contains("fontSize") && params["fontSize"].is_number_integer()) {
                s.fontSize = params["fontSize"].get<int>();
            }
            if (params.contains("opacity") && params["opacity"].is_number_integer()) {
                s.opacity = params["opacity"].get<int>();
            }
            if (params.contains("textColor") && params["textColor"].is_string()) {
                s.textColor = params["textColor"].get<std::string>();
            }
            if (params.contains("backgroundColor") && params["backgroundColor"].is_string()) {
                s.backgroundColor = params["backgroundColor"].get<std::string>();
            }
            if (params.contains("modifierKeycapColor") && params["modifierKeycapColor"].is_string()) {
                s.modifierKeycapColor = params["modifierKeycapColor"].get<std::string>();
            }
            if (params.contains("modifierKeycapOpacity") && params["modifierKeycapOpacity"].is_number_integer()) {
                s.modifierKeycapOpacity = params["modifierKeycapOpacity"].get<int>();
            }
            if (params.contains("modifierTextColor") && params["modifierTextColor"].is_string()) {
                s.modifierTextColor = params["modifierTextColor"].get<std::string>();
            }
            if (params.contains("firstKeyAnim") && params["firstKeyAnim"].is_string()) {
                s.firstKeyAnim = params["firstKeyAnim"].get<std::string>();
            }
            if (params.contains("subsequentKeyAnim") && params["subsequentKeyAnim"].is_string()) {
                s.subsequentKeyAnim = params["subsequentKeyAnim"].get<std::string>();
            }
            if (params.contains("rowCascadeAnim") && params["rowCascadeAnim"].is_boolean()) {
                s.rowCascadeAnim = params["rowCascadeAnim"].get<bool>();
            }
            if (params.contains("exitDriftAnim") && params["exitDriftAnim"].is_boolean()) {
                s.exitDriftAnim = params["exitDriftAnim"].get<bool>();
            }

            overlay.updateSettings(s);
            tools3000::core::KeyboardHook::instance().syncConfigCache();
            return {{"success", true}};
        });

        mb.registerHandler("keycast.resetDefaults", [](const nlohmann::json&) -> nlohmann::json {
            KeycastOverlay::instance().resetDefaults();
            tools3000::core::KeyboardHook::instance().syncConfigCache();
            return {{"success", true}};
        });

        mb.registerHandler("keycast.trigger", [](const nlohmann::json&) -> nlohmann::json {
            KeycastOverlay::instance().pushKey("Win + E");
            return {{"success", true}};
        });

        mb.registerHandler("keycast.setDockingRegion", [](const nlohmann::json& params) -> nlohmann::json {
            bool active = params.value("active", false);
            if (active) {
                int x = params.value("x", 0);
                int y = params.value("y", 0);
                int w = params.value("width", 0);
                int h = params.value("height", 0);
                RECT r{x, y, x + w, y + h};
                KeycastOverlay::instance().setDockingRegion(r, true);
            } else {
                KeycastOverlay::instance().setDockingRegion({}, false);
            }
            return {{"success", true}};
        });

        return true;
    }

    void shutdown() override {
        LOG_INFO("Keycast Plugin shutdown");
        g_pluginShuttingDown.store(true, std::memory_order_release);
        
        if (m_themeSubscription != 0) {
            tools3000::core::EventBus::instance().unsubscribeAndWait(m_themeSubscription);
            m_themeSubscription = 0;
        }
        tools3000::core::MessageBridge::instance().unregisterHandlersByPrefix("keycast.");
        tools3000::core::KeyboardHook::instance().setKeycastKeyInfoCallback(nullptr);
        tools3000::core::KeyboardHook::instance().setKeycastCallback(nullptr);
        
        // Cleanup Overlay UI
        KeycastOverlay::instance().cleanup();
        tools3000::core::WinUtils::trimWorkingSet();
    }

private:
    tools3000::core::SubscriptionId m_themeSubscription = 0;
};

extern "C" __declspec(dllexport) tools3000::core::IPlugin* CreatePlugin() {
    static KeycastPlugin instance;
    return &instance;
}

extern "C" __declspec(dllexport) std::uint32_t GetPluginAbiVersion() {
    return tools3000::core::CurrentPluginAbiVersion;
}

} // namespace tools3000::keycast

BOOL APIENTRY DllMain(HMODULE hModule, DWORD ul_reason_for_call, LPVOID) {
    switch (ul_reason_for_call) {
        case DLL_PROCESS_ATTACH:
            DisableThreadLibraryCalls(hModule);
            break;
        case DLL_PROCESS_DETACH:
            break;
    }
    return TRUE;
}
