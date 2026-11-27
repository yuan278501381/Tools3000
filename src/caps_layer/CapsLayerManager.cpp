// ─────────────────────────────────────────────────────────────────────────────
// CapsLayerManager.cpp — CapsLock 引导键与分应用改键管理器实现
// ─────────────────────────────────────────────────────────────────────────────

#include "caps_layer/CapsLayerManager.h"

#include "core/config/ConfigManager.h"
#include "core/ipc/MessageBridge.h"
#include "core/logger/Logger.h"
#include "core/utils/WinUtils.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <string_view>

namespace {

struct CapsOutputActionFFI {
    uint8_t kind;
    uint8_t mods;
    uint16_t reserved;
    uint32_t vk;
    uint32_t text_index;
};

constexpr uint8_t kActionKeyDown = 1;
constexpr uint8_t kActionKeyUp = 2;
constexpr uint8_t kActionSendCombo = 3;
constexpr uint8_t kActionSendTextIndex = 4;
constexpr uint8_t kActionTapToggleCaps = 5;
constexpr uint8_t kActionTapEscape = 6;

constexpr uint32_t kMaxConsecutiveHookErrors = 5;
constexpr size_t kMaxQueuedTasks = 64;

extern "C" {
CapsEngine* t3k_caps_create();
void t3k_caps_destroy(CapsEngine* ptr);
void t3k_caps_set_enabled(CapsEngine* ptr, uint8_t enabled);
void t3k_caps_reset_state(CapsEngine* ptr);
uint8_t t3k_caps_is_idle(const CapsEngine* ptr);
void t3k_caps_clear_rules(CapsEngine* ptr);
void t3k_caps_set_options(CapsEngine* ptr, uint8_t tap_action_code, uint8_t shift_caps_native);
void t3k_caps_add_excluded_app(CapsEngine* ptr, const uint8_t* exe_utf8, size_t exe_len);
uint32_t t3k_caps_add_text(CapsEngine* ptr, const uint8_t* text_utf8, size_t text_len);
void t3k_caps_add_mapping(
    CapsEngine* ptr,
    const uint8_t* app_exe_utf8,
    size_t app_exe_len,
    uint32_t trigger_vk,
    uint8_t target_type,
    uint8_t target_mods,
    uint32_t target_vk,
    uint32_t text_index);
uint8_t t3k_caps_get_text(
    const CapsEngine* ptr,
    uint32_t text_index,
    const uint8_t** out_ptr,
    size_t* out_len);
uint8_t t3k_caps_process_key(
    CapsEngine* ptr,
    uint32_t vk,
    uint8_t is_down,
    uint8_t mods_mask,
    const uint8_t* fg_exe_utf8,
    size_t fg_exe_len,
    CapsOutputActionFFI* out_actions,
    size_t max_actions,
    size_t* out_count);
}

std::string trimAscii(std::string_view sv) {
    size_t start = 0;
    while (start < sv.size() && std::isspace(static_cast<unsigned char>(sv[start]))) {
        ++start;
    }
    size_t end = sv.size();
    while (end > start && std::isspace(static_cast<unsigned char>(sv[end - 1]))) {
        --end;
    }
    return std::string(sv.substr(start, end - start));
}

std::string toLowerAscii(std::string_view sv) {
    std::string out;
    out.reserve(sv.size());
    for (unsigned char c : sv) {
        out.push_back(static_cast<char>(std::tolower(c)));
    }
    return out;
}

std::string normalizeExeBaseLower(std::string_view raw) {
    std::string s = trimAscii(raw);
    if (!s.empty() && s.front() == '"' && s.back() == '"' && s.size() >= 2) {
        s = s.substr(1, s.size() - 2);
    }
    const size_t pos = s.find_last_of("\\/");
    if (pos != std::string::npos) {
        s = s.substr(pos + 1);
    }
    return toLowerAscii(s);
}

std::string normalizeTapAction(const std::string& raw) {
    const std::string lower = toLowerAscii(raw);
    if (lower == "esc" || lower == "escape") return "esc";
    if (lower == "none" || lower == "disabled") return "none";
    return "toggle_caps";
}

uint8_t tapActionToCode(const std::string& tapAction) {
    const std::string norm = normalizeTapAction(tapAction);
    if (norm == "esc") return 1;
    if (norm == "none") return 2;
    return 0;
}

nlohmann::json makeMappingItem(
    const char* id,
    const char* key,
    const char* targetType,
    const char* targetValue,
    const char* label) {
    return {
        {"id", id},
        {"key", key},
        {"targetType", targetType},
        {"targetValue", targetValue},
        {"label", label},
        {"enabled", true}
    };
}

nlohmann::json normalizeMappingArray(const nlohmann::json& rawArr) {
    nlohmann::json out = nlohmann::json::array();
    if (!rawArr.is_array()) return out;
    size_t idx = 0;
    for (const auto& item : rawArr) {
        if (!item.is_object()) continue;
        const std::string key = item.value("key", item.value("trigger", std::string{}));
        const std::string targetValue = item.value("targetValue", item.value("target", std::string{}));
        if (key.empty() || targetValue.empty()) continue;
        std::string targetType = toLowerAscii(item.value("targetType", std::string("key")));
        if (targetType != "key" && targetType != "combo" && targetType != "text") {
            targetType = "key";
        }
        std::string id = item.value("id", std::string{});
        if (id.empty()) {
            id = "map-" + std::to_string(idx);
        }
        out.push_back({
            {"id", id},
            {"key", key},
            {"targetType", targetType},
            {"targetValue", targetValue},
            {"label", item.value("label", targetValue)},
            {"enabled", item.value("enabled", true)}
        });
        ++idx;
    }
    return out;
}

}  // namespace

namespace tools3000::caps_layer {

CapsLayerManager& CapsLayerManager::instance() {
    static CapsLayerManager inst;
    return inst;
}

CapsLayerManager::~CapsLayerManager() {
    shutdown();
}

nlohmann::json CapsLayerManager::buildPresetGlobalMappings(const std::string& presetId) {
    if (presetId == "arrows_vim") {
        return nlohmann::json::array({
            makeMappingItem("vim-h", "H", "key", "Left", "Left"),
            makeMappingItem("vim-j", "J", "key", "Down", "Down"),
            makeMappingItem("vim-k", "K", "key", "Up", "Up"),
            makeMappingItem("vim-l", "L", "key", "Right", "Right"),
            makeMappingItem("vim-0", "0", "key", "Home", "Home"),
            makeMappingItem("vim-4", "4", "key", "End", "End"),
            makeMappingItem("vim-x", "X", "key", "Delete", "Delete"),
            makeMappingItem("vim-u", "U", "combo", "Ctrl+Z", "Undo"),
            makeMappingItem("vim-w", "W", "combo", "Ctrl+Right", "Next Word"),
            makeMappingItem("vim-b", "B", "combo", "Ctrl+Left", "Prev Word"),
        });
    }
    if (presetId == "arrows_esdf") {
        return nlohmann::json::array({
            makeMappingItem("esdf-e", "E", "key", "Up", "Up"),
            makeMappingItem("esdf-d", "D", "key", "Down", "Down"),
            makeMappingItem("esdf-s", "S", "key", "Left", "Left"),
            makeMappingItem("esdf-f", "F", "key", "Right", "Right"),
            makeMappingItem("esdf-w", "W", "key", "Home", "Home"),
            makeMappingItem("esdf-r", "R", "key", "End", "End"),
            makeMappingItem("esdf-a", "A", "key", "Backspace", "Backspace"),
            makeMappingItem("esdf-g", "G", "key", "Delete", "Delete"),
        });
    }
    return nlohmann::json::array({
        makeMappingItem("ijkl-i", "I", "key", "Up", "Up"),
        makeMappingItem("ijkl-k", "K", "key", "Down", "Down"),
        makeMappingItem("ijkl-j", "J", "key", "Left", "Left"),
        makeMappingItem("ijkl-l", "L", "key", "Right", "Right"),
        makeMappingItem("ijkl-u", "U", "key", "Home", "Home"),
        makeMappingItem("ijkl-o", "O", "key", "End", "End"),
        makeMappingItem("ijkl-h", "H", "key", "Backspace", "Backspace"),
        makeMappingItem("ijkl-semi", ";", "key", "Delete", "Delete"),
        makeMappingItem("ijkl-p", "P", "key", "PgUp", "Page Up"),
        makeMappingItem("ijkl-n", "N", "key", "PgDn", "Page Down"),
    });
}

nlohmann::json CapsLayerManager::buildDefaultSettingsJson() {
    return {
        {"enabled", false},
        {"tapAction", "toggle_caps"},
        {"shiftCapsTogglesNative", true},
        {"excludedApps", nlohmann::json::array({
            "cs2.exe", "dota2.exe", "valorant.exe", "mstsc.exe", "vmware.exe"
        })},
        {"globalMappings", buildPresetGlobalMappings("arrows_ijkl")},
        {"appProfiles", nlohmann::json::array({
            {
                {"id", "vscode"},
                {"name", "VS Code"},
                {"processName", "code.exe"},
                {"enabled", true},
                {"mappings", nlohmann::json::array({
                    makeMappingItem("vscode-r", "R", "key", "F2", "Rename Symbol"),
                    makeMappingItem("vscode-d", "D", "combo", "Ctrl+D", "Select Next Occurrence"),
                    makeMappingItem("vscode-f", "F", "combo", "Ctrl+Shift+F", "Search Workspace")
                })}
            }
        })}
    };
}

uint32_t CapsLayerManager::parseKeyNameToVk(const std::string& keyName) {
    const std::string s = toLowerAscii(trimAscii(keyName));
    if (s.empty()) return 0;

    if (s.size() == 1) {
        const unsigned char ch = static_cast<unsigned char>(s[0]);
        if (ch >= 'a' && ch <= 'z') return static_cast<uint32_t>('A' + (ch - 'a'));
        if (ch >= '0' && ch <= '9') return static_cast<uint32_t>(ch);
        switch (ch) {
            case ';': return VK_OEM_1;
            case '=': return VK_OEM_PLUS;
            case ',': return VK_OEM_COMMA;
            case '-': return VK_OEM_MINUS;
            case '.': return VK_OEM_PERIOD;
            case '/': return VK_OEM_2;
            case '`': return VK_OEM_3;
            case '[': return VK_OEM_4;
            case '\\': return VK_OEM_5;
            case ']': return VK_OEM_6;
            case '\'': return VK_OEM_7;
            default: return 0;
        }
    }

    if (s == "up" || s == "arrowup") return VK_UP;
    if (s == "down" || s == "arrowdown") return VK_DOWN;
    if (s == "left" || s == "arrowleft") return VK_LEFT;
    if (s == "right" || s == "arrowright") return VK_RIGHT;
    if (s == "home") return VK_HOME;
    if (s == "end") return VK_END;
    if (s == "pageup" || s == "pgup" || s == "prior") return VK_PRIOR;
    if (s == "pagedown" || s == "pgdn" || s == "next") return VK_NEXT;
    if (s == "backspace" || s == "back" || s == "bs") return VK_BACK;
    if (s == "delete" || s == "del") return VK_DELETE;
    if (s == "insert" || s == "ins") return VK_INSERT;
    if (s == "enter" || s == "return") return VK_RETURN;
    if (s == "escape" || s == "esc") return VK_ESCAPE;
    if (s == "tab") return VK_TAB;
    if (s == "space" || s == "spacebar") return VK_SPACE;
    if (s == "capslock" || s == "caps") return VK_CAPITAL;
    if (s == "printscreen" || s == "prtsc") return VK_SNAPSHOT;
    if (s == "pause") return VK_PAUSE;
    if (s == "apps" || s == "menu") return VK_APPS;
    if (s == "semicolon") return VK_OEM_1;
    if (s == "equal" || s == "plus") return VK_OEM_PLUS;
    if (s == "comma") return VK_OEM_COMMA;
    if (s == "minus") return VK_OEM_MINUS;
    if (s == "period" || s == "dot") return VK_OEM_PERIOD;
    if (s == "slash") return VK_OEM_2;
    if (s == "backquote" || s == "grave" || s == "tilde") return VK_OEM_3;
    if (s == "bracketleft") return VK_OEM_4;
    if (s == "backslash") return VK_OEM_5;
    if (s == "bracketright") return VK_OEM_6;
    if (s == "quote" || s == "apostrophe") return VK_OEM_7;
    if (s == "volumeup") return VK_VOLUME_UP;
    if (s == "volumedown") return VK_VOLUME_DOWN;
    if (s == "volumemute") return VK_VOLUME_MUTE;
    if (s == "mediaplaypause") return VK_MEDIA_PLAY_PAUSE;
    if (s == "medianext") return VK_MEDIA_NEXT_TRACK;
    if (s == "mediaprev") return VK_MEDIA_PREV_TRACK;

    if (s.size() >= 2 && s[0] == 'f') {
        try {
            const int fn = std::stoi(s.substr(1));
            if (fn >= 1 && fn <= 24) {
                return static_cast<uint32_t>(VK_F1 + (fn - 1));
            }
        } catch (...) {
        }
    }

    if (s.rfind("0x", 0) == 0) {
        try {
            const unsigned long v = std::stoul(s.substr(2), nullptr, 16);
            if (v > 0 && v < 256) return static_cast<uint32_t>(v);
        } catch (...) {
        }
    }

    return 0;
}

bool CapsLayerManager::parseComboString(const std::string& comboStr, uint8_t& outMods, uint32_t& outVk) {
    outMods = 0;
    outVk = 0;
    if (comboStr.empty()) return false;

    size_t pos = 0;
    while (pos <= comboStr.size()) {
        size_t plus = comboStr.find('+', pos);
        std::string token;
        if (plus == std::string::npos) {
            token = trimAscii(std::string_view(comboStr).substr(pos));
            pos = comboStr.size() + 1;
        } else {
            token = trimAscii(std::string_view(comboStr).substr(pos, plus - pos));
            pos = plus + 1;
        }
        if (token.empty()) continue;
        const std::string lower = toLowerAscii(token);
        if (lower == "ctrl" || lower == "control") {
            outMods |= CAPS_MOD_CTRL;
        } else if (lower == "alt" || lower == "menu") {
            outMods |= CAPS_MOD_ALT;
        } else if (lower == "shift") {
            outMods |= CAPS_MOD_SHIFT;
        } else if (lower == "win" || lower == "meta" || lower == "super") {
            outMods |= CAPS_MOD_WIN;
        } else {
            outVk = parseKeyNameToVk(token);
        }
    }
    return outVk != 0;
}

void CapsLayerManager::ensureEngineLocked() {
    if (!m_engine) {
        m_engine = t3k_caps_create();
    }
}

void CapsLayerManager::loadFromConfigLocked() {
    m_settings = buildDefaultSettingsJson();
    auto& cfg = tools3000::core::ConfigManager::instance();
    const auto saved = cfg.get<nlohmann::json>("/capsLayer", nlohmann::json::object());
    if (saved.is_object()) {
        if (saved.contains("enabled") && saved["enabled"].is_boolean()) {
            m_settings["enabled"] = saved["enabled"].get<bool>();
        }
        if (saved.contains("tapAction") && saved["tapAction"].is_string()) {
            m_settings["tapAction"] = normalizeTapAction(saved["tapAction"].get<std::string>());
        }
        if (saved.contains("shiftCapsTogglesNative") && saved["shiftCapsTogglesNative"].is_boolean()) {
            m_settings["shiftCapsTogglesNative"] = saved["shiftCapsTogglesNative"].get<bool>();
        } else if (saved.contains("shiftCapsNative") && saved["shiftCapsNative"].is_boolean()) {
            m_settings["shiftCapsTogglesNative"] = saved["shiftCapsNative"].get<bool>();
        }
        if (saved.contains("excludedApps") && saved["excludedApps"].is_array()) {
            m_settings["excludedApps"] = saved["excludedApps"];
        }
        if (saved.contains("globalMappings") && saved["globalMappings"].is_array()) {
            m_settings["globalMappings"] = normalizeMappingArray(saved["globalMappings"]);
        }
        if (saved.contains("appProfiles") && saved["appProfiles"].is_array()) {
            nlohmann::json profiles = nlohmann::json::array();
            for (const auto& p : saved["appProfiles"]) {
                if (!p.is_object()) continue;
                nlohmann::json prof = p;
                if (prof.contains("mappings") && prof["mappings"].is_array()) {
                    prof["mappings"] = normalizeMappingArray(prof["mappings"]);
                } else {
                    prof["mappings"] = nlohmann::json::array();
                }
                profiles.push_back(std::move(prof));
            }
            m_settings["appProfiles"] = std::move(profiles);
        }
    }
    bool enabled = m_settings.value("enabled", false);
    if (cfg.has("/plugins/caps_layer/enabled")) {
        enabled = cfg.get<bool>("/plugins/caps_layer/enabled", enabled);
    }
    m_settings["enabled"] = enabled;
}

void CapsLayerManager::saveToConfigLocked() const {
    auto& cfg = tools3000::core::ConfigManager::instance();
    nlohmann::json patch = nlohmann::json::object();
    patch["capsLayer"] = m_settings;
    patch["plugins"]["caps_layer"]["enabled"] = m_settings.value("enabled", false);
    cfg.mergePatch(patch, "/capsLayer");
}

void CapsLayerManager::rebuildRustRulesLocked() {
    std::lock_guard engLock(m_engineMutex);
    const bool enabled = m_settings.value("enabled", false);
    if (!enabled && !m_engine) {
        m_enabled.store(false, std::memory_order_release);
        return;
    }

    ensureEngineLocked();
    t3k_caps_clear_rules(m_engine);
    t3k_caps_set_options(
        m_engine,
        tapActionToCode(m_settings.value("tapAction", std::string("toggle_caps"))),
        m_settings.value("shiftCapsTogglesNative", true) ? 1 : 0);

    if (m_settings.contains("excludedApps") && m_settings["excludedApps"].is_array()) {
        for (const auto& item : m_settings["excludedApps"]) {
            if (!item.is_string()) continue;
            const std::string exe = item.get<std::string>();
            if (!exe.empty()) {
                t3k_caps_add_excluded_app(
                    m_engine,
                    reinterpret_cast<const uint8_t*>(exe.data()),
                    exe.size());
            }
        }
    }

    auto registerMappingList = [&](const std::string& appExe, const nlohmann::json& list) {
        if (!list.is_array()) return;
        for (const auto& item : list) {
            if (!item.is_object()) continue;
            if (!item.value("enabled", true)) continue;

            uint32_t triggerVk = 0;
            if (item.contains("triggerVk") && item["triggerVk"].is_number_unsigned()) {
                triggerVk = item["triggerVk"].get<uint32_t>();
            } else {
                const std::string keyStr = item.value("key", item.value("trigger", std::string{}));
                triggerVk = parseKeyNameToVk(keyStr);
            }
            if (triggerVk == 0) continue;

            const std::string targetType = toLowerAscii(item.value("targetType", std::string("key")));
            const std::string targetStr = item.value("targetValue", item.value("target", std::string{}));

            const uint8_t* appPtr = appExe.empty()
                ? nullptr
                : reinterpret_cast<const uint8_t*>(appExe.data());
            const size_t appLen = appExe.size();

            if (targetType == "text") {
                if (targetStr.empty()) continue;
                const uint32_t idx = t3k_caps_add_text(
                    m_engine,
                    reinterpret_cast<const uint8_t*>(targetStr.data()),
                    targetStr.size());
                if (idx != UINT32_MAX) {
                    t3k_caps_add_mapping(m_engine, appPtr, appLen, triggerVk, 2, 0, 0, idx);
                }
            } else if (targetType == "combo") {
                uint8_t mods = 0;
                uint32_t vk = 0;
                if (parseComboString(targetStr, mods, vk)) {
                    t3k_caps_add_mapping(m_engine, appPtr, appLen, triggerVk, 1, mods, vk, 0);
                }
            } else {
                uint32_t targetVk = 0;
                if (item.contains("targetVk") && item["targetVk"].is_number_unsigned()) {
                    targetVk = item["targetVk"].get<uint32_t>();
                } else {
                    targetVk = parseKeyNameToVk(targetStr);
                }
                if (targetVk != 0) {
                    t3k_caps_add_mapping(m_engine, appPtr, appLen, triggerVk, 0, 0, targetVk, 0);
                }
            }
        }
    };

    if (m_settings.contains("globalMappings")) {
        registerMappingList("", m_settings["globalMappings"]);
    }
    if (m_settings.contains("appProfiles") && m_settings["appProfiles"].is_array()) {
        for (const auto& prof : m_settings["appProfiles"]) {
            if (!prof.is_object() || !prof.value("enabled", true)) continue;
            const std::string proc = normalizeExeBaseLower(prof.value("processName", std::string{}));
            if (proc.empty()) continue;
            if (prof.contains("mappings")) {
                registerMappingList(proc, prof["mappings"]);
            }
        }
    }

    t3k_caps_set_enabled(m_engine, enabled ? 1 : 0);
    m_capsOrRolloverActive.store(false, std::memory_order_release);
    m_circuitBreakerTripped.store(false, std::memory_order_release);
    m_consecutiveHookErrors.store(0, std::memory_order_release);
    m_enabled.store(enabled, std::memory_order_release);
}

void CapsLayerManager::startInjectorWorkerLocked() {
    std::lock_guard wLock(m_workerLifecycleMutex);
    if (m_workerRunning.load(std::memory_order_acquire)) {
        return;
    }
    m_workerRunning.store(true, std::memory_order_release);
    m_injectorThread = std::thread(&CapsLayerManager::injectorWorkerLoop, this);
}

void CapsLayerManager::stopInjectorWorker() {
    std::thread toJoin;
    {
        std::lock_guard wLock(m_workerLifecycleMutex);
        if (!m_workerRunning.exchange(false, std::memory_order_acq_rel)) {
            return;
        }
        {
            std::lock_guard tLock(m_taskMutex);
            m_taskQueue.clear();
        }
        m_taskCv.notify_all();
        toJoin = std::move(m_injectorThread);
    }
    if (toJoin.joinable() && toJoin.get_id() != std::this_thread::get_id()) {
        toJoin.join();
    }
}

void CapsLayerManager::initialize() {
    bool shouldRun = false;
    {
        std::lock_guard lock(m_stateMutex);
        if (m_initialized) return;
        loadFromConfigLocked();
        rebuildRustRulesLocked();
        shouldRun = m_settings.value("enabled", false);
        m_initialized = true;
    }
    if (shouldRun) {
        startInjectorWorkerLocked();
        LOG_INFO("[CapsLayer] Initialized in enabled mode");
    } else {
        LOG_DEBUG("[CapsLayer] Initialized in default-off zero-footprint mode");
    }
}

void CapsLayerManager::shutdown() {
    m_enabled.store(false, std::memory_order_release);
    m_capsOrRolloverActive.store(false, std::memory_order_release);
    stopInjectorWorker();
    {
        std::lock_guard engLock(m_engineMutex);
        if (m_engine) {
            t3k_caps_destroy(m_engine);
            m_engine = nullptr;
        }
    }
    {
        std::lock_guard lock(m_stateMutex);
        m_initialized = false;
    }
}

bool CapsLayerManager::setEnabled(bool enabled) {
    {
        std::lock_guard lock(m_stateMutex);
        if (!m_initialized) {
            loadFromConfigLocked();
            m_initialized = true;
        }
        m_settings["enabled"] = enabled;
        saveToConfigLocked();
        rebuildRustRulesLocked();
    }
    if (enabled) {
        startInjectorWorkerLocked();
        LOG_INFO("[CapsLayer] Enabled by user");
    } else {
        stopInjectorWorker();
        LOG_INFO("[CapsLayer] Disabled by user; worker stopped and zero-footprint restored");
    }
    pushStateChangedEvents(enabled);
    return true;
}

void CapsLayerManager::updateForegroundProcess(const std::string& processName) {
    const std::string norm = normalizeExeBaseLower(processName);
    std::lock_guard lock(m_fgMutex);
    m_cachedFgExeLower = norm;
}

void CapsLayerManager::resetRuntimeState() {
    m_capsOrRolloverActive.store(false, std::memory_order_release);
    std::lock_guard engLock(m_engineMutex);
    if (m_engine) {
        t3k_caps_reset_state(m_engine);
    }
}

nlohmann::json CapsLayerManager::getSettingsJson() const {
    std::lock_guard lock(m_stateMutex);
    nlohmann::json out = m_settings.is_object() && !m_settings.empty()
        ? m_settings
        : buildDefaultSettingsJson();
    out["circuitBreakerTripped"] = m_circuitBreakerTripped.load(std::memory_order_acquire);
    return out;
}

nlohmann::json CapsLayerManager::updateSettings(const nlohmann::json& params) {
    if (!params.is_object()) {
        return {{"success", false}, {"error", "invalid params"}};
    }

    bool enabled = false;
    bool prevEnabled = false;
    nlohmann::json snapshot;
    {
        std::lock_guard lock(m_stateMutex);
        if (!m_initialized) {
            loadFromConfigLocked();
            m_initialized = true;
        }
        prevEnabled = m_settings.value("enabled", false);

        if (params.contains("enabled") && params["enabled"].is_boolean()) {
            m_settings["enabled"] = params["enabled"].get<bool>();
        }
        if (params.contains("tapAction") && params["tapAction"].is_string()) {
            m_settings["tapAction"] = normalizeTapAction(params["tapAction"].get<std::string>());
        }
        if (params.contains("shiftCapsTogglesNative") && params["shiftCapsTogglesNative"].is_boolean()) {
            m_settings["shiftCapsTogglesNative"] = params["shiftCapsTogglesNative"].get<bool>();
        } else if (params.contains("shiftCapsNative") && params["shiftCapsNative"].is_boolean()) {
            m_settings["shiftCapsTogglesNative"] = params["shiftCapsNative"].get<bool>();
        }
        if (params.contains("excludedApps") && params["excludedApps"].is_array()) {
            nlohmann::json cleaned = nlohmann::json::array();
            for (const auto& item : params["excludedApps"]) {
                if (!item.is_string()) continue;
                const std::string exe = normalizeExeBaseLower(item.get<std::string>());
                if (!exe.empty()) cleaned.push_back(exe);
            }
            m_settings["excludedApps"] = std::move(cleaned);
        }
        if (params.contains("globalMappings") && params["globalMappings"].is_array()) {
            m_settings["globalMappings"] = normalizeMappingArray(params["globalMappings"]);
        }
        if (params.contains("appProfiles") && params["appProfiles"].is_array()) {
            nlohmann::json profiles = nlohmann::json::array();
            for (const auto& p : params["appProfiles"]) {
                if (!p.is_object()) continue;
                nlohmann::json prof = p;
                prof["processName"] = normalizeExeBaseLower(prof.value("processName", std::string{}));
                prof["mappings"] = normalizeMappingArray(prof.value("mappings", nlohmann::json::array()));
                profiles.push_back(std::move(prof));
            }
            m_settings["appProfiles"] = std::move(profiles);
        }

        enabled = m_settings.value("enabled", false);
        saveToConfigLocked();
        rebuildRustRulesLocked();
        snapshot = m_settings;
    }

    if (enabled) {
        startInjectorWorkerLocked();
    } else {
        stopInjectorWorker();
    }
    if (enabled != prevEnabled) {
        pushStateChangedEvents(enabled);
    } else {
        tools3000::core::MessageBridge::instance().pushEvent("capsLayer:stateChanged", snapshot);
    }
    snapshot["success"] = true;
    return snapshot;
}

nlohmann::json CapsLayerManager::applyPreset(const std::string& presetId) {
    const std::string id = (presetId == "arrows_vim" || presetId == "arrows_esdf")
        ? presetId
        : std::string("arrows_ijkl");

    nlohmann::json snapshot;
    {
        std::lock_guard lock(m_stateMutex);
        if (!m_initialized) {
            loadFromConfigLocked();
            m_initialized = true;
        }
        m_settings["globalMappings"] = buildPresetGlobalMappings(id);
        saveToConfigLocked();
        rebuildRustRulesLocked();
        snapshot = m_settings;
    }
    tools3000::core::MessageBridge::instance().pushEvent("capsLayer:stateChanged", snapshot);
    LOG_INFO("[CapsLayer] Applied preset: {}", id);
    snapshot["success"] = true;
    snapshot["presetId"] = id;
    return snapshot;
}

void CapsLayerManager::pushStateChangedEvents(bool enabled) const {
    auto& mb = tools3000::core::MessageBridge::instance();
    mb.pushEvent("capsLayer:stateChanged", getSettingsJson());
    mb.pushEvent("plugins:changed", {{"id", "caps_layer"}, {"enabled", enabled}});
}

bool CapsLayerManager::isExtendedVirtualKey(uint32_t vk) noexcept {
    switch (vk) {
        case VK_UP:
        case VK_DOWN:
        case VK_LEFT:
        case VK_RIGHT:
        case VK_HOME:
        case VK_END:
        case VK_PRIOR:
        case VK_NEXT:
        case VK_INSERT:
        case VK_DELETE:
        case VK_DIVIDE:
        case VK_NUMLOCK:
        case VK_RCONTROL:
        case VK_RMENU:
        case VK_LWIN:
        case VK_RWIN:
        case VK_APPS:
        case VK_SNAPSHOT:
            return true;
        default:
            return false;
    }
}

INPUT CapsLayerManager::makeKeyboardInput(uint32_t vk, bool isKeyUp) noexcept {
    INPUT in{};
    in.type = INPUT_KEYBOARD;
    in.ki.wVk = static_cast<WORD>(vk);
    in.ki.wScan = static_cast<WORD>(MapVirtualKeyW(vk, MAPVK_VK_TO_VSC));
    in.ki.dwFlags = (isKeyUp ? KEYEVENTF_KEYUP : 0u) |
                    (isExtendedVirtualKey(vk) ? KEYEVENTF_EXTENDEDKEY : 0u);
    in.ki.dwExtraInfo = kCapsLayerInjectedTag;
    return in;
}

void CapsLayerManager::sendSingleKeyImmediate(uint32_t vk, bool isKeyUp) noexcept {
    if (vk == 0) return;
    INPUT in = makeKeyboardInput(vk, isKeyUp);
    SendInput(1, &in, sizeof(INPUT));
}

void CapsLayerManager::enqueueAsyncTask(AsyncInjectionTask task) {
    {
        std::lock_guard lock(m_taskMutex);
        if (m_taskQueue.size() >= kMaxQueuedTasks) {
            m_taskQueue.pop_front();
        }
        m_taskQueue.push_back(std::move(task));
    }
    m_taskCv.notify_one();
}

void CapsLayerManager::injectorWorkerLoop() {
    while (m_workerRunning.load(std::memory_order_acquire)) {
        AsyncInjectionTask task;
        {
            std::unique_lock lock(m_taskMutex);
            m_taskCv.wait(lock, [this]() {
                return !m_workerRunning.load(std::memory_order_acquire) || !m_taskQueue.empty();
            });
            if (!m_workerRunning.load(std::memory_order_acquire)) {
                break;
            }
            if (m_taskQueue.empty()) continue;
            task = std::move(m_taskQueue.front());
            m_taskQueue.pop_front();
        }
        executeAsyncTask(task);
    }
}

void CapsLayerManager::executeAsyncTask(const AsyncInjectionTask& task) noexcept {
    try {
        std::vector<INPUT> seq;
        if (task.kind == kActionTapToggleCaps) {
            seq.push_back(makeKeyboardInput(VK_CAPITAL, false));
            seq.push_back(makeKeyboardInput(VK_CAPITAL, true));
        } else if (task.kind == kActionTapEscape) {
            seq.push_back(makeKeyboardInput(VK_ESCAPE, false));
            seq.push_back(makeKeyboardInput(VK_ESCAPE, true));
        } else if (task.kind == kActionSendCombo && task.vk != 0) {
            std::vector<uint32_t> modVks;
            if (task.mods & CAPS_MOD_CTRL) modVks.push_back(VK_LCONTROL);
            if (task.mods & CAPS_MOD_ALT) modVks.push_back(VK_LMENU);
            if (task.mods & CAPS_MOD_SHIFT) modVks.push_back(VK_LSHIFT);
            if (task.mods & CAPS_MOD_WIN) modVks.push_back(VK_LWIN);

            for (uint32_t mv : modVks) {
                seq.push_back(makeKeyboardInput(mv, false));
            }
            seq.push_back(makeKeyboardInput(task.vk, false));
            seq.push_back(makeKeyboardInput(task.vk, true));
            for (auto it = modVks.rbegin(); it != modVks.rend(); ++it) {
                seq.push_back(makeKeyboardInput(*it, true));
            }
        } else if (task.kind == kActionSendTextIndex && !task.textW.empty()) {
            seq.reserve(task.textW.size() * 2);
            for (wchar_t wc : task.textW) {
                INPUT down{};
                down.type = INPUT_KEYBOARD;
                down.ki.wScan = static_cast<WORD>(wc);
                down.ki.dwFlags = KEYEVENTF_UNICODE;
                down.ki.dwExtraInfo = kCapsLayerInjectedTag;
                INPUT up = down;
                up.ki.dwFlags = KEYEVENTF_UNICODE | KEYEVENTF_KEYUP;
                seq.push_back(down);
                seq.push_back(up);
            }
        }

        if (!seq.empty()) {
            SendInput(static_cast<UINT>(seq.size()), seq.data(), sizeof(INPUT));
        }
    } catch (...) {
    }
}

bool CapsLayerManager::handleLowLevelKey(const KBDLLHOOKSTRUCT& data, WPARAM wParam) noexcept {
    if (!isEnabled()) {
        return false;
    }
    if (data.dwExtraInfo == kCapsLayerInjectedTag) {
        return false;
    }

    // 极速零锁直通路径：日常打字只要未按住 CapsLock 且无滚键残留，
    // 对所有普通按键（含空格/字母/数字）仅需 1 次原子读 (<2ns)，0 互斥锁、0 字符串拷贝、0 Rust FFI。
    if (data.vkCode != VK_CAPITAL &&
        !m_capsOrRolloverActive.load(std::memory_order_acquire)) {
        return false;
    }

    const bool isDown = (wParam == WM_KEYDOWN || wParam == WM_SYSKEYDOWN);
    const bool isUp = (wParam == WM_KEYUP || wParam == WM_SYSKEYUP);
    if (!isDown && !isUp) {
        return false;
    }

    try {
        std::unique_lock engLock(m_engineMutex, std::try_to_lock);
        if (!engLock.owns_lock() || !m_engine) {
            return false;
        }

        std::string fgExe;
        {
            std::unique_lock fgLock(m_fgMutex, std::try_to_lock);
            if (fgLock.owns_lock()) {
                fgExe = m_cachedFgExeLower;
            }
        }

        uint8_t modsMask = 0;
        if ((GetAsyncKeyState(VK_CONTROL) & 0x8000) != 0) modsMask |= CAPS_MOD_CTRL;
        if ((GetAsyncKeyState(VK_MENU) & 0x8000) != 0) modsMask |= CAPS_MOD_ALT;
        if ((GetAsyncKeyState(VK_SHIFT) & 0x8000) != 0) modsMask |= CAPS_MOD_SHIFT;
        if (((GetAsyncKeyState(VK_LWIN) & 0x8000) != 0) || ((GetAsyncKeyState(VK_RWIN) & 0x8000) != 0)) {
            modsMask |= CAPS_MOD_WIN;
        }

        std::array<CapsOutputActionFFI, 4> actions{};
        size_t actionCount = 0;
        const uint8_t consumed = t3k_caps_process_key(
            m_engine,
            data.vkCode,
            isDown ? 1 : 0,
            modsMask,
            fgExe.empty() ? nullptr : reinterpret_cast<const uint8_t*>(fgExe.data()),
            fgExe.size(),
            actions.data(),
            actions.size(),
            &actionCount);
        m_capsOrRolloverActive.store(t3k_caps_is_idle(m_engine) == 0, std::memory_order_release);

        std::vector<AsyncInjectionTask> deferredTasks;
        for (size_t i = 0; i < actionCount; ++i) {
            const auto& act = actions[i];
            if (act.kind == kActionKeyDown) {
                sendSingleKeyImmediate(act.vk, false);
            } else if (act.kind == kActionKeyUp) {
                sendSingleKeyImmediate(act.vk, true);
            } else if (act.kind == kActionSendCombo ||
                       act.kind == kActionTapToggleCaps ||
                       act.kind == kActionTapEscape) {
                AsyncInjectionTask t;
                t.kind = act.kind;
                t.mods = act.mods;
                t.vk = act.vk;
                deferredTasks.push_back(std::move(t));
            } else if (act.kind == kActionSendTextIndex) {
                const uint8_t* ptr = nullptr;
                size_t len = 0;
                if (t3k_caps_get_text(m_engine, act.text_index, &ptr, &len) != 0 && ptr && len > 0) {
                    AsyncInjectionTask t;
                    t.kind = kActionSendTextIndex;
                    t.textW = tools3000::core::WinUtils::utf8ToWstring(
                        std::string(reinterpret_cast<const char*>(ptr), len));
                    deferredTasks.push_back(std::move(t));
                }
            }
        }

        engLock.unlock();

        for (auto& task : deferredTasks) {
            enqueueAsyncTask(std::move(task));
        }

        if (m_consecutiveHookErrors.load(std::memory_order_relaxed) != 0) {
            m_consecutiveHookErrors.store(0, std::memory_order_relaxed);
        }
        return consumed != 0;
    } catch (...) {
        const uint32_t errs = m_consecutiveHookErrors.fetch_add(1, std::memory_order_acq_rel) + 1;
        if (errs >= kMaxConsecutiveHookErrors) {
            m_circuitBreakerTripped.store(true, std::memory_order_release);
            m_capsOrRolloverActive.store(false, std::memory_order_release);
            m_enabled.store(false, std::memory_order_release);
            LOG_ERROR("[CapsLayer] Circuit breaker tripped after {} hook faults; auto-disabled to protect host", errs);
        }
        return false;
    }
}

}  // namespace tools3000::caps_layer
