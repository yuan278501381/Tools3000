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

HHOOK g_altGrObserverHook = nullptr;
DWORD g_altGrObserverThreadId = 0;
std::atomic<bool> g_realLCtrlDown{false};
std::atomic<bool> g_realRCtrlDown{false};
std::atomic<bool> g_syntheticAltGrCtrlDown{false};
std::atomic<DWORD> g_lastNonModVk{0};
std::atomic<DWORD> g_lastNonModScan{0};
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

LRESULT CALLBACK altGrKeyboardObserverProc(int nCode, WPARAM wParam, LPARAM lParam) noexcept {
    if (nCode == HC_ACTION && lParam != 0) {
        const auto* kbd = reinterpret_cast<const KBDLLHOOKSTRUCT*>(lParam);
        const bool isDown = (wParam == WM_KEYDOWN || wParam == WM_SYSKEYDOWN);
        const bool isUp = (wParam == WM_KEYUP || wParam == WM_SYSKEYUP);
        if (kbd->vkCode == VK_LCONTROL) {
            // European keyboard layouts emit a synthetic Left-Ctrl with scanCode 0x21D (0x0200 prefix) before Right-Alt (AltGr).
            const bool isSyntheticAltGr = (kbd->scanCode == 0x21D) || ((kbd->scanCode & 0xFF00u) == 0x0200u);
            if (isSyntheticAltGr) {
                if (isDown) g_syntheticAltGrCtrlDown.store(true, std::memory_order_relaxed);
                else if (isUp) g_syntheticAltGrCtrlDown.store(false, std::memory_order_relaxed);
            } else {
                if (isDown) {
                    g_realLCtrlDown.store(true, std::memory_order_relaxed);
                } else if (isUp) {
                    g_realLCtrlDown.store(false, std::memory_order_relaxed);
                    g_syntheticAltGrCtrlDown.store(false, std::memory_order_relaxed);
                }
            }
        } else if (kbd->vkCode == VK_RCONTROL) {
            if (isDown) g_realRCtrlDown.store(true, std::memory_order_relaxed);
            else if (isUp) g_realRCtrlDown.store(false, std::memory_order_relaxed);
        } else if (kbd->vkCode == VK_RMENU && isUp) {
            g_syntheticAltGrCtrlDown.store(false, std::memory_order_relaxed);
        } else if (isDown &&
                   kbd->vkCode != VK_CONTROL && kbd->vkCode != VK_MENU &&
                   kbd->vkCode != VK_LMENU && kbd->vkCode != VK_RMENU &&
                   kbd->vkCode != VK_SHIFT && kbd->vkCode != VK_LSHIFT &&
                   kbd->vkCode != VK_RSHIFT && kbd->vkCode != VK_LWIN &&
                   kbd->vkCode != VK_RWIN) {
            g_lastNonModVk.store(kbd->vkCode, std::memory_order_relaxed);
            g_lastNonModScan.store(kbd->scanCode, std::memory_order_relaxed);
        }
    }
    return CallNextHookEx(nullptr, nCode, wParam, lParam);
}

void installAltGrObserver() {
    auto installFn = []() {
        if (!g_altGrObserverHook) {
            g_altGrObserverHook = SetWindowsHookExW(
                WH_KEYBOARD_LL,
                altGrKeyboardObserverProc,
                GetModuleHandleW(nullptr),
                0);
            g_altGrObserverThreadId = g_altGrObserverHook ? GetCurrentThreadId() : 0;
        }
    };
    auto& disp = tools3000::core::MainThreadDispatcher::instance();
    if (!disp.isInitialized() || disp.isOwnerThread()) {
        installFn();
    } else {
        auto done = std::make_shared<std::promise<void>>();
        auto fut = done->get_future();
        if (disp.post([installFn, done]() {
                installFn();
                done->set_value();
            })) {
            fut.wait_for(std::chrono::milliseconds(500));
        }
    }
}

void uninstallAltGrObserver() {
    auto uninstallFn = []() {
        if (g_altGrObserverHook) {
            UnhookWindowsHookEx(g_altGrObserverHook);
            g_altGrObserverHook = nullptr;
            g_altGrObserverThreadId = 0;
        }
    };
    if (!g_altGrObserverHook) return;
    if (GetCurrentThreadId() == g_altGrObserverThreadId) {
        uninstallFn();
        return;
    }
    auto& disp = tools3000::core::MainThreadDispatcher::instance();
    if (disp.isInitialized()) {
        auto done = std::make_shared<std::promise<void>>();
        auto fut = done->get_future();
        if (disp.post([uninstallFn, done]() {
                uninstallFn();
                done->set_value();
            })) {
            fut.wait_for(std::chrono::milliseconds(500));
        }
    }
}

} // namespace

class KeycastPlugin : public tools3000::core::IPlugin {
public:
    const char* getName() const override { return "Keycast"; }
    const char* getVersion() const override { return tools3000::version::String; }

    bool initialize() override {
        LOG_INFO("Keycast Plugin initialize");
        
        g_pluginShuttingDown.store(false, std::memory_order_release);
        g_realLCtrlDown.store(false, std::memory_order_relaxed);
        g_realRCtrlDown.store(false, std::memory_order_relaxed);
        g_syntheticAltGrCtrlDown.store(false, std::memory_order_relaxed);

        // Initialize Overlay UI
        if (!KeycastOverlay::instance().init()) {
            LOG_ERROR("Failed to init KeycastOverlay");
            return false;
        }

        installAltGrObserver();

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
            const bool hasCtrl = containsToken("Ctrl");
            const bool hasAlt = containsToken("Alt");
            const bool hasShift = containsToken("Shift");
            const bool hasWin = containsToken("Win");

            if (!hasCtrl) {
                g_syntheticAltGrCtrlDown.store(false, std::memory_order_relaxed);
                g_realLCtrlDown.store(false, std::memory_order_relaxed);
                g_realRCtrlDown.store(false, std::memory_order_relaxed);
            }

            const bool isPureAltGr =
                hasCtrl && hasAlt && !hasWin &&
                g_syntheticAltGrCtrlDown.load(std::memory_order_relaxed) &&
                !g_realLCtrlDown.load(std::memory_order_relaxed) &&
                !g_realRCtrlDown.load(std::memory_order_relaxed);

            if (isPureAltGr) {
                const std::string lastToken = info.tokens.empty() ? std::string{} : info.tokens.back();
                const bool isModOnly = lastToken.empty() ||
                                       lastToken == "Ctrl" || lastToken == "Alt" ||
                                       lastToken == "Shift" || lastToken == "Win";
                if (isModOnly) {
                    return;
                }
                if (isPrintableCharacterToken(lastToken)) {
                    const DWORD vk = g_lastNonModVk.load(std::memory_order_relaxed);
                    const DWORD sc = g_lastNonModScan.load(std::memory_order_relaxed);
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
                {"showKeyboard", s.showKeyboard},
                {"filterMode", s.filterMode},
                {"includeFunctionKeys", s.includeFunctionKeys},
                {"position", s.position},
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
            if (params.contains("showKeyboard") && params["showKeyboard"].is_boolean()) {
                s.showKeyboard = params["showKeyboard"].get<bool>();
            }
            if (params.contains("filterMode") && params["filterMode"].is_string()) {
                s.filterMode = params["filterMode"].get<std::string>();
            }
            if (params.contains("includeFunctionKeys") && params["includeFunctionKeys"].is_boolean()) {
                s.includeFunctionKeys = params["includeFunctionKeys"].get<bool>();
            }
            if (params.contains("position") && params["position"].is_string()) {
                s.position = params["position"].get<std::string>();
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
        uninstallAltGrObserver();
        
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
