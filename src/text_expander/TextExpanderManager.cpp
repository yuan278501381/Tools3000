// -----------------------------------------------------------------------------
// TextExpanderManager.cpp -- 系统级文本扩展器引擎实现
// -----------------------------------------------------------------------------

#include "text_expander/TextExpanderManager.h"

#include "clipboard/ClipboardNativeDrag.h"
#include "core/config/ConfigManager.h"
#include "core/events/EventBus.h"
#include "core/hotkey/HotkeyManager.h"
#include "core/hotkey/KeyboardHook.h"
#include "core/ipc/MessageBridge.h"
#include "core/logger/Logger.h"
#include "core/utils/WinUtils.h"

#include <bcrypt.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstring>
#include <ctime>
#include <fstream>
#include <iterator>
#include <sstream>

namespace tools3000::text_expander {

namespace {

constexpr ULONG_PTR kInjectedExtraInfoTag = 0x54334B54u;  // "T3KT"
constexpr ULONGLONG kTypingIdleResetMs = 5000;
constexpr DWORD kRestoreClipboardDelayMs = 140;
constexpr size_t kMaxPendingJobs = 8;

/// 注册并缓存三项 Windows 官方剪贴板隐私排除格式
struct PrivateClipboardFormats {
    UINT excludeMonitor = 0;
    UINT canIncludeHistory = 0;
    UINT viewerIgnore = 0;

    static const PrivateClipboardFormats& get() {
        static const PrivateClipboardFormats formats = []() {
            PrivateClipboardFormats f;
            f.excludeMonitor = RegisterClipboardFormatW(L"ExcludeClipboardContentFromMonitorProcessing");
            f.canIncludeHistory = RegisterClipboardFormatW(L"CanIncludeInClipboardHistory");
            f.viewerIgnore = RegisterClipboardFormatW(L"Clipboard Viewer Ignore");
            return f;
        }();
        return formats;
    }
};

bool openClipboardWithRetry(int maxAttempts = 8, DWORD retryDelayMs = 10) noexcept {
    for (int i = 0; i < maxAttempts; ++i) {
        if (OpenClipboard(nullptr) != FALSE) {
            return true;
        }
        Sleep(retryDelayMs);
    }
    return false;
}

bool setClipboardDwordFormat(UINT format, DWORD value) noexcept {
    if (format == 0) return false;
    HGLOBAL hMem = GlobalAlloc(GMEM_MOVEABLE, sizeof(DWORD));
    if (!hMem) return false;
    void* ptr = GlobalLock(hMem);
    if (!ptr) {
        GlobalFree(hMem);
        return false;
    }
    std::memcpy(ptr, &value, sizeof(DWORD));
    GlobalUnlock(hMem);
    if (!SetClipboardData(format, hMem)) {
        GlobalFree(hMem);
        return false;
    }
    return true;
}

bool isModifierVirtualKey(DWORD vk) noexcept {
    switch (vk) {
        case VK_SHIFT:
        case VK_LSHIFT:
        case VK_RSHIFT:
        case VK_CONTROL:
        case VK_LCONTROL:
        case VK_RCONTROL:
        case VK_MENU:
        case VK_LMENU:
        case VK_RMENU:
        case VK_LWIN:
        case VK_RWIN:
        case VK_CAPITAL:
        case VK_NUMLOCK:
        case VK_SCROLL:
            return true;
        default:
            return false;
    }
}

bool isNavigationOrResetKey(DWORD vk) noexcept {
    switch (vk) {
        case VK_LEFT:
        case VK_RIGHT:
        case VK_UP:
        case VK_DOWN:
        case VK_HOME:
        case VK_END:
        case VK_PRIOR:
        case VK_NEXT:
        case VK_ESCAPE:
        case VK_DELETE:
        case VK_INSERT:
        case VK_APPS:
            return true;
        default:
            return (vk >= VK_F1 && vk <= VK_F24);
    }
}

INPUT makeVkInput(WORD vk, bool keyUp, bool extended = false) noexcept {
    INPUT in{};
    in.type = INPUT_KEYBOARD;
    in.ki.wVk = vk;
    in.ki.wScan = static_cast<WORD>(MapVirtualKeyW(vk, MAPVK_VK_TO_VSC));
    in.ki.dwFlags = (keyUp ? KEYEVENTF_KEYUP : 0u) | (extended ? KEYEVENTF_EXTENDEDKEY : 0u);
    in.ki.dwExtraInfo = kInjectedExtraInfoTag;
    return in;
}

INPUT makeUnicodeInput(wchar_t ch, bool keyUp) noexcept {
    INPUT in{};
    in.type = INPUT_KEYBOARD;
    in.ki.wVk = 0;
    in.ki.wScan = static_cast<WORD>(ch);
    in.ki.dwFlags = KEYEVENTF_UNICODE | (keyUp ? KEYEVENTF_KEYUP : 0u);
    in.ki.dwExtraInfo = kInjectedExtraInfoTag;
    return in;
}

std::string buildIsoTimestampUtc() {
    const std::time_t now = std::time(nullptr);
    std::tm utcTm{};
    gmtime_s(&utcTm, &now);
    char buf[32]{};
    std::snprintf(
        buf,
        sizeof(buf),
        "%04d-%02d-%02dT%02d:%02d:%02dZ",
        utcTm.tm_year + 1900,
        utcTm.tm_mon + 1,
        utcTm.tm_mday,
        utcTm.tm_hour,
        utcTm.tm_min,
        utcTm.tm_sec);
    return std::string(buf);
}

std::wstring queryCurrentSystemUserNameWide() {
    wchar_t buf[256]{};
    DWORD len = static_cast<DWORD>(std::size(buf));
    if (GetUserNameW(buf, &len) && len > 0) {
        return std::wstring(buf);
    }
    return L"user";
}

std::string resolveEffectiveLocaleCode() {
    const std::string configured =
        tools3000::core::ConfigManager::instance().get<std::string>("/general/language", "auto");
    if (!configured.empty() && configured != "auto") {
        return configured;
    }
    wchar_t localeName[LOCALE_NAME_MAX_LENGTH]{};
    if (GetUserDefaultLocaleName(localeName, LOCALE_NAME_MAX_LENGTH) > 0) {
        return detail::utf16ToUtf8(localeName);
    }
    return "en";
}

std::wstring generateRandomUuidV4Wide() {
    std::array<uint8_t, 16> bytes{};
    if (BCryptGenRandom(
            nullptr,
            bytes.data(),
            static_cast<ULONG>(bytes.size()),
            BCRYPT_USE_SYSTEM_PREFERRED_RNG) != 0) {
        const uint64_t seed = static_cast<uint64_t>(GetTickCount64()) ^
                              reinterpret_cast<uintptr_t>(&bytes);
        return detail::fallbackDeterministicUuidV4(seed);
    }
    bytes[6] = static_cast<uint8_t>((bytes[6] & 0x0Fu) | 0x40u);
    bytes[8] = static_cast<uint8_t>((bytes[8] & 0x3Fu) | 0x80u);
    wchar_t out[40]{};
    std::swprintf(
        out,
        std::size(out),
        L"%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x%02x%02x%02x",
        bytes[0], bytes[1], bytes[2], bytes[3],
        bytes[4], bytes[5],
        bytes[6], bytes[7],
        bytes[8], bytes[9],
        bytes[10], bytes[11], bytes[12], bytes[13], bytes[14], bytes[15]);
    return std::wstring(out);
}

bool hasDuplicateTrigger(
    const std::vector<TextExpanderRule>& rules,
    const TextExpanderRule& candidate) {
    const std::wstring candW = detail::utf8ToUtf16(candidate.trigger);

    for (const auto& existing : rules) {
        if (!candidate.id.empty() && existing.id == candidate.id) continue;
        if (existing.mode != candidate.mode) continue;

        const std::wstring existW = detail::utf8ToUtf16(existing.trigger);
        const bool strictCase = candidate.caseSensitive && existing.caseSensitive;
        const bool strictFullwidth = candidate.fullwidthSensitive && existing.fullwidthSensitive;
        if (detail::normalizeRuleString(existW, strictCase, strictFullwidth) ==
            detail::normalizeRuleString(candW, strictCase, strictFullwidth)) {
            return true;
        }
    }
    return false;
}

bool validateRuleCandidate(const TextExpanderRule& rule, std::string& outError) {
    if (rule.trigger.empty()) {
        outError = "empty_trigger";
        return false;
    }
    const std::wstring trigW = detail::utf8ToUtf16(rule.trigger);
    if (trigW.empty()) {
        outError = "empty_trigger";
        return false;
    }
    if (trigW.size() > TextExpanderMatcher::kMaxBufferLength) {
        outError = "trigger_too_long";
        return false;
    }
    for (wchar_t ch : trigW) {
        if (ch == L'\r' || ch == L'\n') {
            outError = "trigger_invalid_newline";
            return false;
        }
    }
    return true;
}

}  // namespace

TextExpanderManager& TextExpanderManager::instance() {
    static TextExpanderManager inst;
    return inst;
}

TextExpanderManager::~TextExpanderManager() {
    shutdown();
}

void TextExpanderManager::setRulesFilePathForTesting(const std::filesystem::path& path) {
    std::lock_guard lock(m_stateMutex);
    m_rulesFilePath = path;
}

void TextExpanderManager::setOnRuleExpandedCallback(
    std::function<void(const std::string&, uint32_t)> cb) {
    std::lock_guard lock(m_callbackMutex);
    m_onRuleExpanded = std::move(cb);
}

void TextExpanderManager::initialize() {
    bool shouldStart = false;
    {
        std::lock_guard lock(m_stateMutex);
        if (m_initialized) return;
        if (m_rulesFilePath.empty()) {
            m_rulesFilePath = tools3000::core::WinUtils::getConfigDirectory() / L"text_expander_rules.json";
        }
        loadSettingsFromConfigLocked();
        loadRulesFromDiskLocked();
        syncMatcherRulesLocked();
        m_enabled.store(m_settings.enabled, std::memory_order_release);
        m_policyEpoch.fetch_add(1, std::memory_order_acq_rel);
        m_initialized = true;
        shouldStart = m_settings.enabled;
    }

    if (shouldStart) {
        startRuntimeWorkers();
        LOG_INFO("TextExpander initialized and active");
    } else {
        LOG_INFO("TextExpander initialized in opt-in disabled state (zero hooks, zero threads)");
    }
}

void TextExpanderManager::shutdown() {
    m_enabled.store(false, std::memory_order_release);
    stopRuntimeWorkers();
    {
        std::lock_guard lock(m_matcherMutex);
        m_matcher.clear();
    }
    {
        std::lock_guard lock(m_stateMutex);
        m_initialized = false;
    }
    LOG_INFO("TextExpander shut down cleanly");
}

bool TextExpanderManager::isEnabled() const noexcept {
    return m_enabled.load(std::memory_order_acquire);
}

bool TextExpanderManager::setEnabled(bool enabled) {
    bool changed = false;
    {
        std::lock_guard lock(m_stateMutex);
        if (!m_initialized) {
            if (m_rulesFilePath.empty()) {
                m_rulesFilePath = tools3000::core::WinUtils::getConfigDirectory() / L"text_expander_rules.json";
            }
            loadSettingsFromConfigLocked();
            loadRulesFromDiskLocked();
            syncMatcherRulesLocked();
            m_initialized = true;
        }
        changed = (m_settings.enabled != enabled);
        m_settings.enabled = enabled;
        m_enabled.store(enabled, std::memory_order_release);
        m_policyEpoch.fetch_add(1, std::memory_order_acq_rel);
        saveSettingsToConfigLocked();
    }

    if (enabled) {
        startRuntimeWorkers();
        LOG_INFO("TextExpander enabled: global low-level hook active");
    } else {
        stopRuntimeWorkers();
        {
            std::lock_guard lock(m_matcherMutex);
            m_matcher.clear();
        }
        LOG_INFO("TextExpander disabled: global hook and worker released");
    }

    if (changed) {
        pushStateChangedEvents(enabled);
    }
    return true;
}

nlohmann::json TextExpanderManager::getSettingsJson() const {
    std::lock_guard lock(m_stateMutex);
    return nlohmann::json{
        {"enabled", m_settings.enabled},
        {"autoExcludeSensitiveApps", m_settings.autoExcludeSensitiveApps},
        {"restoreClipboard", m_settings.restoreClipboard},
        {"excludedProcesses", m_settings.excludedProcesses},
        {"defaultSensitiveProcesses", defaultSensitiveProcesses()},
    };
}

nlohmann::json TextExpanderManager::updateSettings(const nlohmann::json& params) {
    if (!params.is_object()) {
        return {{"success", false}, {"error", "invalid_params"}};
    }

    bool previousEnabled = false;
    bool nextEnabled = false;
    nlohmann::json snapshot;
    {
        std::lock_guard lock(m_stateMutex);
        if (!m_initialized) {
            if (m_rulesFilePath.empty()) {
                m_rulesFilePath = tools3000::core::WinUtils::getConfigDirectory() / L"text_expander_rules.json";
            }
            loadSettingsFromConfigLocked();
            loadRulesFromDiskLocked();
            syncMatcherRulesLocked();
            m_initialized = true;
        }

        previousEnabled = m_settings.enabled;

        if (params.contains("enabled") && params["enabled"].is_boolean()) {
            m_settings.enabled = params["enabled"].get<bool>();
        }
        if (params.contains("autoExcludeSensitiveApps") && params["autoExcludeSensitiveApps"].is_boolean()) {
            m_settings.autoExcludeSensitiveApps = params["autoExcludeSensitiveApps"].get<bool>();
        }
        if (params.contains("restoreClipboard") && params["restoreClipboard"].is_boolean()) {
            m_settings.restoreClipboard = params["restoreClipboard"].get<bool>();
        }
        if (params.contains("excludedProcesses")) {
            if (params["excludedProcesses"].is_array()) {
                std::vector<std::string> raw;
                for (const auto& item : params["excludedProcesses"]) {
                    if (item.is_string()) {
                        raw.push_back(item.get<std::string>());
                    }
                }
                m_settings.excludedProcesses = normalizeProcessList(raw);
            } else if (params["excludedProcesses"].is_string()) {
                m_settings.excludedProcesses =
                    parseExcludedProcessString(params["excludedProcesses"].get<std::string>());
            }
        }

        nextEnabled = m_settings.enabled;
        m_enabled.store(nextEnabled, std::memory_order_release);
        m_policyEpoch.fetch_add(1, std::memory_order_acq_rel);
        saveSettingsToConfigLocked();

        snapshot = nlohmann::json{
            {"enabled", m_settings.enabled},
            {"autoExcludeSensitiveApps", m_settings.autoExcludeSensitiveApps},
            {"restoreClipboard", m_settings.restoreClipboard},
            {"excludedProcesses", m_settings.excludedProcesses},
            {"defaultSensitiveProcesses", defaultSensitiveProcesses()},
        };
    }

    if (nextEnabled) {
        startRuntimeWorkers();
    } else {
        stopRuntimeWorkers();
        std::lock_guard lock(m_matcherMutex);
        m_matcher.clear();
    }

    if (previousEnabled != nextEnabled) {
        pushStateChangedEvents(nextEnabled);
    }

    snapshot["success"] = true;
    return snapshot;
}

nlohmann::json TextExpanderManager::listRulesJson() const {
    std::lock_guard lock(m_stateMutex);
    nlohmann::json arr = nlohmann::json::array();
    for (const auto& rule : m_rules) {
        arr.push_back(ruleToJson(rule));
    }
    return arr;
}

nlohmann::json TextExpanderManager::upsertRule(const nlohmann::json& params) {
    TextExpanderRule candidate;
    if (!ruleFromJson(params, candidate)) {
        return {{"success", false}, {"error", "invalid_rule"}};
    }

    std::string validationError;
    if (!validateRuleCandidate(candidate, validationError)) {
        return {{"success", false}, {"error", validationError}};
    }

    std::lock_guard lock(m_stateMutex);
    if (m_rulesFilePath.empty()) {
        m_rulesFilePath = tools3000::core::WinUtils::getConfigDirectory() / L"text_expander_rules.json";
    }
    if (hasDuplicateTrigger(m_rules, candidate)) {
        return {{"success", false}, {"error", "duplicate_trigger"}};
    }

    bool updated = false;
    if (!candidate.id.empty()) {
        for (auto& existing : m_rules) {
            if (existing.id == candidate.id) {
                if (!params.contains("hitCount")) {
                    candidate.hitCount = existing.hitCount;
                }
                existing = candidate;
                updated = true;
                break;
            }
        }
    }

    if (!updated) {
        if (candidate.id.empty()) {
            candidate.id = generateRuleId();
        }
        m_rules.push_back(candidate);
    }

    syncMatcherRulesLocked();
    const bool saved = saveRulesToDiskLocked();

    nlohmann::json rulesArr = nlohmann::json::array();
    for (const auto& r : m_rules) {
        rulesArr.push_back(ruleToJson(r));
    }

    return {
        {"success", saved},
        {"rule", ruleToJson(candidate)},
        {"rules", std::move(rulesArr)},
        {"error", saved ? "" : "save_failed"},
    };
}

nlohmann::json TextExpanderManager::deleteRule(const std::string& id) {
    if (id.empty()) {
        return {{"success", false}, {"error", "empty_id"}};
    }

    std::lock_guard lock(m_stateMutex);
    const auto oldSize = m_rules.size();
    m_rules.erase(
        std::remove_if(m_rules.begin(), m_rules.end(), [&](const TextExpanderRule& r) {
            return r.id == id;
        }),
        m_rules.end());

    const bool removed = (m_rules.size() != oldSize);
    bool saved = true;
    if (removed) {
        syncMatcherRulesLocked();
        saved = saveRulesToDiskLocked();
    }

    nlohmann::json rulesArr = nlohmann::json::array();
    for (const auto& r : m_rules) {
        rulesArr.push_back(ruleToJson(r));
    }

    return {
        {"success", removed && saved},
        {"removed", removed},
        {"rules", std::move(rulesArr)},
    };
}

nlohmann::json TextExpanderManager::toggleRule(const std::string& id, bool enabled) {
    if (id.empty()) {
        return {{"success", false}, {"error", "empty_id"}};
    }

    std::lock_guard lock(m_stateMutex);
    bool found = false;
    for (auto& r : m_rules) {
        if (r.id == id) {
            r.enabled = enabled;
            found = true;
            break;
        }
    }

    bool saved = false;
    if (found) {
        syncMatcherRulesLocked();
        saved = saveRulesToDiskLocked();
    }

    nlohmann::json rulesArr = nlohmann::json::array();
    for (const auto& r : m_rules) {
        rulesArr.push_back(ruleToJson(r));
    }

    return {
        {"success", found && saved},
        {"rules", std::move(rulesArr)},
        {"error", found ? (saved ? "" : "save_failed") : "not_found"},
    };
}

nlohmann::json TextExpanderManager::importRules(const nlohmann::json& params) {
    const nlohmann::json* arrayPtr = nullptr;
    bool replaceAll = false;

    if (params.is_array()) {
        arrayPtr = &params;
    } else if (params.is_object() && params.contains("rules") && params["rules"].is_array()) {
        arrayPtr = &params["rules"];
        const std::string mode = toLowerAscii(params.value("mode", std::string{"merge"}));
        replaceAll = (mode == "replace");
    } else {
        return {{"success", false}, {"error", "invalid_import_payload"}};
    }

    std::lock_guard lock(m_stateMutex);
    if (m_rulesFilePath.empty()) {
        m_rulesFilePath = tools3000::core::WinUtils::getConfigDirectory() / L"text_expander_rules.json";
    }
    std::vector<TextExpanderRule> nextRules = replaceAll ? std::vector<TextExpanderRule>{} : m_rules;
    size_t imported = 0;
    size_t skipped = 0;

    for (const auto& item : *arrayPtr) {
        TextExpanderRule parsed;
        if (!ruleFromJson(item, parsed)) {
            ++skipped;
            continue;
        }
        std::string err;
        if (!validateRuleCandidate(parsed, err)) {
            ++skipped;
            continue;
        }
        if (parsed.id.empty()) {
            parsed.id = generateRuleId();
        }

        // 若同 ID 或同触发词已存在则覆盖更新，否则追加
        bool merged = false;
        const std::wstring parsedLower = detail::toLowerWide(detail::utf8ToUtf16(parsed.trigger));
        for (auto& existing : nextRules) {
            const bool sameId = existing.id == parsed.id;
            const bool sameTrigger = existing.mode == parsed.mode &&
                detail::toLowerWide(detail::utf8ToUtf16(existing.trigger)) == parsedLower;
            if (sameId || sameTrigger) {
                parsed.id = existing.id;
                existing = parsed;
                merged = true;
                break;
            }
        }
        if (!merged) {
            nextRules.push_back(std::move(parsed));
        }
        ++imported;
    }

    m_rules = std::move(nextRules);
    syncMatcherRulesLocked();
    const bool saved = saveRulesToDiskLocked();

    nlohmann::json rulesArr = nlohmann::json::array();
    for (const auto& r : m_rules) {
        rulesArr.push_back(ruleToJson(r));
    }

    LOG_INFO("TextExpander imported rules: imported={}, skipped={}, replace={}", imported, skipped, replaceAll);
    return {
        {"success", saved},
        {"imported", imported},
        {"skipped", skipped},
        {"rules", std::move(rulesArr)},
    };
}

nlohmann::json TextExpanderManager::exportRulesJson() const {
    std::lock_guard lock(m_stateMutex);
    nlohmann::json arr = nlohmann::json::array();
    for (const auto& rule : m_rules) {
        arr.push_back(ruleToJson(rule));
    }
    return {
        {"version", 1},
        {"exportedAt", buildIsoTimestampUtc()},
        {"rules", std::move(arr)},
    };
}

void TextExpanderManager::loadSettingsFromConfigLocked() {
    auto& cfg = tools3000::core::ConfigManager::instance();
    bool enabled = true;
    if (cfg.has("/plugins/text_expander/enabled")) {
        enabled = cfg.get<bool>("/plugins/text_expander/enabled", true);
    } else if (cfg.has("/textExpander/enabled")) {
        enabled = cfg.get<bool>("/textExpander/enabled", true);
    }
    m_settings.enabled = enabled;
    m_settings.autoExcludeSensitiveApps = cfg.get<bool>("/textExpander/autoExcludeSensitiveApps", true);
    m_settings.restoreClipboard = cfg.get<bool>("/textExpander/restoreClipboard", true);
    auto rawExcluded = cfg.get<std::vector<std::string>>("/textExpander/excludedProcesses", {});
    m_settings.excludedProcesses = normalizeProcessList(rawExcluded);
}

void TextExpanderManager::saveSettingsToConfigLocked() const {
    auto& cfg = tools3000::core::ConfigManager::instance();
    nlohmann::json patch = {
        {"plugins", {
            {"text_expander", {
                {"enabled", m_settings.enabled},
            }},
        }},
        {"textExpander", {
            {"enabled", m_settings.enabled},
            {"autoExcludeSensitiveApps", m_settings.autoExcludeSensitiveApps},
            {"restoreClipboard", m_settings.restoreClipboard},
            {"excludedProcesses", m_settings.excludedProcesses},
        }},
    };
    cfg.mergePatch(patch, "/textExpander");
}

void TextExpanderManager::loadRulesFromDiskLocked() {
    std::error_code ec;
    if (!std::filesystem::exists(m_rulesFilePath, ec)) {
        m_rules = defaultSampleRules();
        if (!saveRulesToDiskLocked()) {
            LOG_WARN("TextExpander failed to persist initial sample rules file");
        }
        return;
    }

    try {
        std::ifstream ifs(m_rulesFilePath, std::ios::binary);
        if (!ifs.is_open()) {
            LOG_WARN("TextExpander could not open rules file; falling back to sample rules in memory");
            m_rules = defaultSampleRules();
            return;
        }
        std::string content((std::istreambuf_iterator<char>(ifs)), std::istreambuf_iterator<char>());
        if (content.empty()) {
            m_rules = defaultSampleRules();
            return;
        }

        const nlohmann::json parsed = nlohmann::json::parse(content);
        const nlohmann::json* listPtr = nullptr;
        if (parsed.is_array()) {
            listPtr = &parsed;
        } else if (parsed.is_object() && parsed.contains("rules") && parsed["rules"].is_array()) {
            listPtr = &parsed["rules"];
        }

        if (!listPtr) {
            LOG_WARN("TextExpander rules file has unexpected JSON root; keeping empty list");
            m_rules.clear();
            return;
        }

        m_rules.clear();
        for (const auto& item : *listPtr) {
            TextExpanderRule rule;
            if (!ruleFromJson(item, rule)) continue;
            std::string err;
            if (!validateRuleCandidate(rule, err)) continue;
            if (rule.id.empty()) {
                rule.id = generateRuleId();
            }
            m_rules.push_back(std::move(rule));
        }
    } catch (const std::exception& e) {
        LOG_ERROR("TextExpander failed to parse rules file: {}", e.what());
        m_rules.clear();
    }
}

bool TextExpanderManager::saveRulesToDiskLocked() const {
    if (m_rulesFilePath.empty()) return false;
    std::error_code ec;
    const auto parent = m_rulesFilePath.parent_path();
    if (!parent.empty()) {
        std::filesystem::create_directories(parent, ec);
    }

    nlohmann::json arr = nlohmann::json::array();
    for (const auto& rule : m_rules) {
        arr.push_back(ruleToJson(rule));
    }
    const nlohmann::json doc = {
        {"version", 1},
        {"rules", std::move(arr)},
    };
    const std::string serialized = doc.dump(2);
    return tools3000::core::WinUtils::atomicWriteFileWithFlush(m_rulesFilePath.wstring(), serialized);
}

void TextExpanderManager::syncMatcherRulesLocked() {
    std::lock_guard matcherLock(m_matcherMutex);
    m_matcher.setRules(m_rules);
}

void TextExpanderManager::startRuntimeWorkers() {
    std::lock_guard lifecycleLock(m_lifecycleMutex);

    if (m_mouseActivitySubId == 0) {
        m_mouseActivitySubId = tools3000::core::EventBus::instance().subscribe<tools3000::core::MouseActivityEvent>(
            [this](const tools3000::core::MouseActivityEvent& ev) {
                if (ev.button >= 0) {
                    m_mouseClicked.store(true, std::memory_order_relaxed);
                }
            });
    }

    if (!m_workerThread.joinable()) {
        m_workerRunning.store(true, std::memory_order_release);
        m_workerThread = std::thread([this]() { expansionWorkerLoop(); });
    }

    m_lastCheckedHwnd = nullptr;
    m_lastCheckedPolicyEpoch = 0;
    m_lastHwndExcluded = false;
    m_lastKeyTickMs = 0;
    m_mouseClicked.store(false, std::memory_order_release);
    m_foregroundDirty.store(true, std::memory_order_release);

    // 统一复用宿主唯一高优先级 KeyboardHook 独立输入线程，彻底消除进程内第 2 条 WH_KEYBOARD_LL 钩子
    auto& kbHook = tools3000::core::KeyboardHook::instance();
    kbHook.setTextExpanderObserver([this](const KBDLLHOOKSTRUCT& data, WPARAM wParam) {
        if ((wParam == WM_KEYDOWN || wParam == WM_SYSKEYDOWN) &&
            m_enabled.load(std::memory_order_acquire) &&
            !m_expanding.load(std::memory_order_acquire) &&
            (data.flags & LLKHF_INJECTED) == 0 &&
            data.dwExtraInfo != kInjectedExtraInfoTag) {
            handleLowLevelKeyDown(data);
        }
    });
    if (!kbHook.isInstalled()) {
        if (!kbHook.install()) {
            LOG_ERROR("TextExpander failed to ensure shared KeyboardHook active");
        }
    }
}

void TextExpanderManager::stopRuntimeWorkers() {
    std::thread workerToJoin;
    uint64_t subToUnsub = 0;

    // 优先解除共享 KeyboardHook 观察者绑定，避免新按键进入
    tools3000::core::KeyboardHook::instance().setTextExpanderObserver(nullptr);

    {
        std::lock_guard lifecycleLock(m_lifecycleMutex);
        subToUnsub = m_mouseActivitySubId;
        m_mouseActivitySubId = 0;

        m_workerRunning.store(false, std::memory_order_release);
        {
            std::lock_guard jobLock(m_jobMutex);
            m_jobQueue.clear();
        }
        m_jobCv.notify_all();
        if (m_workerThread.joinable()) {
            workerToJoin = std::move(m_workerThread);
        }
    }

    if (subToUnsub != 0) {
        tools3000::core::EventBus::instance().unsubscribeAndWait(subToUnsub);
    }
    if (workerToJoin.joinable()) {
        workerToJoin.join();
    }
}

void TextExpanderManager::notifyForegroundChanged() {
    if (!m_enabled.load(std::memory_order_acquire)) {
        return;
    }
    handleForegroundChanged();
}

void TextExpanderManager::handleForegroundChanged() {
    m_foregroundDirty.store(true, std::memory_order_release);
    std::lock_guard lock(m_matcherMutex);
    m_matcher.clear();
}

bool TextExpanderManager::isProcessExcluded(const std::string& lowerExeName) const {
    if (lowerExeName.empty()) return false;
    std::lock_guard lock(m_stateMutex);
    if (m_settings.autoExcludeSensitiveApps) {
        const auto& defaults = defaultSensitiveProcesses();
        if (std::find(defaults.begin(), defaults.end(), lowerExeName) != defaults.end()) {
            return true;
        }
    }
    return std::find(
               m_settings.excludedProcesses.begin(),
               m_settings.excludedProcesses.end(),
               lowerExeName) != m_settings.excludedProcesses.end();
}

bool TextExpanderManager::evaluateForegroundWindowExcluded(HWND fgHwnd) {
    if (!fgHwnd || !IsWindow(fgHwnd)) {
        return false;
    }

    // 始终放行 Tools3000 自身进程，确保设置页“实时体验沙盒输入框”可直接试敲展开
    DWORD pid = 0;
    GetWindowThreadProcessId(fgHwnd, &pid);
    if (pid == GetCurrentProcessId()) {
        return false;
    }

    const std::string exeName = tools3000::core::WinUtils::getProcessNameFromWindow(fgHwnd);
    if (exeName.empty()) return false;
    return isProcessExcluded(toLowerAscii(exeName));
}

void TextExpanderManager::handleLowLevelKeyDown(const KBDLLHOOKSTRUCT& data) {
    // 1. 快捷键录制态、全屏游戏静默态或远控直通态自动让位并清空缓冲
    auto& hk = tools3000::core::HotkeyManager::instance();
    if (hk.isPaused() || hk.isFullscreenSuspended() || hk.isRemoteSuspended()) {
        std::lock_guard lock(m_matcherMutex);
        m_matcher.clear();
        return;
    }

    const DWORD vk = data.vkCode;
    if (isModifierVirtualKey(vk)) {
        return;
    }

    // 2. 检测鼠标点击、窗口切换、或长时间打字停顿，及时清空旧缓冲防串词
    const HWND fg = GetForegroundWindow();
    const uint64_t currentEpoch = m_policyEpoch.load(std::memory_order_acquire);
    const ULONGLONG nowTick = GetTickCount64();

    bool resetBuffer = m_mouseClicked.exchange(false, std::memory_order_acq_rel);
    if (m_foregroundDirty.exchange(false, std::memory_order_acq_rel) || fg != m_lastCheckedHwnd) {
        resetBuffer = true;
        m_lastCheckedHwnd = fg;
        m_lastCheckedPolicyEpoch = currentEpoch;
        m_lastHwndExcluded = evaluateForegroundWindowExcluded(fg);
    } else if (m_lastCheckedPolicyEpoch != currentEpoch) {
        m_lastCheckedPolicyEpoch = currentEpoch;
        m_lastHwndExcluded = evaluateForegroundWindowExcluded(fg);
    }

    if (m_lastKeyTickMs != 0 && (nowTick - m_lastKeyTickMs) > kTypingIdleResetMs) {
        resetBuffer = true;
    }
    m_lastKeyTickMs = nowTick;

    if (resetBuffer) {
        std::lock_guard lock(m_matcherMutex);
        m_matcher.clear();
    }

    if (m_lastHwndExcluded) {
        return;
    }

    // 3. 修饰键组合检测（Ctrl / Alt / Win 快捷键清空缓冲；兼容欧洲键盘 AltGr = Ctrl+Alt）
    const bool ctrlDown = (GetAsyncKeyState(VK_CONTROL) & 0x8000) != 0;
    const bool altDown = (GetAsyncKeyState(VK_MENU) & 0x8000) != 0;
    const bool winDown = ((GetAsyncKeyState(VK_LWIN) & 0x8000) != 0) ||
                         ((GetAsyncKeyState(VK_RWIN) & 0x8000) != 0);
    const bool isAltGr = ctrlDown && altDown && !winDown;
    if (winDown || ((ctrlDown || altDown) && !isAltGr)) {
        std::lock_guard lock(m_matcherMutex);
        m_matcher.clear();
        return;
    }

    // 4. 退格修正与导航/控制键清空
    if (vk == VK_BACK) {
        std::lock_guard lock(m_matcherMutex);
        m_matcher.backspace();
        return;
    }
    if (isNavigationOrResetKey(vk)) {
        std::lock_guard lock(m_matcherMutex);
        m_matcher.clear();
        return;
    }

    // 5. 虚拟键码 -> Unicode 字符无副作用翻译（ToUnicodeEx flags=0x04 不破坏死键状态）
    wchar_t translated[4]{};
    int translatedLen = 0;

    if (vk == VK_SPACE) {
        translated[0] = L' ';
        translatedLen = 1;
    } else if (vk == VK_TAB) {
        translated[0] = L'\t';
        translatedLen = 1;
    } else if (vk == VK_RETURN) {
        translated[0] = L'\n';
        translatedLen = 1;
    } else {
        BYTE keyState[256]{};
        if ((GetAsyncKeyState(VK_SHIFT) & 0x8000) != 0) keyState[VK_SHIFT] = 0x80;
        if ((GetAsyncKeyState(VK_LSHIFT) & 0x8000) != 0) keyState[VK_LSHIFT] = 0x80;
        if ((GetAsyncKeyState(VK_RSHIFT) & 0x8000) != 0) keyState[VK_RSHIFT] = 0x80;
        if ((GetKeyState(VK_CAPITAL) & 0x0001) != 0) keyState[VK_CAPITAL] = 0x01;
        if (isAltGr) {
            keyState[VK_CONTROL] = 0x80;
            keyState[VK_MENU] = 0x80;
            keyState[VK_RMENU] = 0x80;
        }
        const DWORD fgTid = fg ? GetWindowThreadProcessId(fg, nullptr) : 0;
        const HKL hkl = GetKeyboardLayout(fgTid);
        translatedLen = ToUnicodeEx(
            static_cast<UINT>(vk),
            static_cast<UINT>(data.scanCode),
            keyState,
            translated,
            static_cast<int>(std::size(translated)),
            0x04u,
            hkl);
    }

    if (translatedLen <= 0) {
        return;
    }

    std::optional<MatchResult> match;
    {
        std::lock_guard lock(m_matcherMutex);
        for (int i = 0; i < translatedLen; ++i) {
            match = m_matcher.pushChar(translated[i]);
            if (match.has_value()) {
                break;
            }
        }
    }

    if (!match.has_value()) {
        return;
    }

    bool restoreClip = true;
    {
        std::lock_guard stateLock(m_stateMutex);
        restoreClip = m_settings.restoreClipboard;
    }

    ExpansionJob job;
    job.ruleId = match->rule.id;
    job.eraseCount = match->eraseCount;
    job.replacementW = detail::utf8ToUtf16(match->rule.replacement);
    job.restoreClipboard = restoreClip;
    job.triggeredByDelimiter = match->triggeredByDelimiter;
    job.keepDelimiter = match->rule.keepDelimiter;
    job.delimiterChar = match->delimiterChar;
    job.targetFgHwnd = fg;

    {
        std::lock_guard jobLock(m_jobMutex);
        if (m_jobQueue.size() < kMaxPendingJobs) {
            m_jobQueue.push_back(std::move(job));
        }
    }
    m_jobCv.notify_one();
}

void TextExpanderManager::expansionWorkerLoop() {
    while (m_workerRunning.load(std::memory_order_acquire)) {
        ExpansionJob job;
        {
            std::unique_lock lock(m_jobMutex);
            m_jobCv.wait(lock, [this]() {
                return !m_workerRunning.load(std::memory_order_acquire) || !m_jobQueue.empty();
            });
            if (!m_workerRunning.load(std::memory_order_acquire)) {
                break;
            }
            job = std::move(m_jobQueue.front());
            m_jobQueue.pop_front();
        }

        executeExpansionJob(job);
    }
}

void TextExpanderManager::executeExpansionJob(const ExpansionJob& job) {
    struct ExpandingGuard {
        std::atomic<bool>& flag;
        explicit ExpandingGuard(std::atomic<bool>& f) : flag(f) {
            flag.store(true, std::memory_order_release);
        }
        ~ExpandingGuard() {
            flag.store(false, std::memory_order_release);
        }
    } guard(m_expanding);

    // 1. 短暂让步 12ms 等待触发尾字符到达目标应用，并等待 Shift 等修饰键自然抬起
    Sleep(12);
    waitModifiersReleased(120);

    // 2. 若模板需要 {{clipboard}} 或将使用剪贴板粘贴法，先读取当前剪贴板文本
    const bool needsClipboardMacro = TextExpanderMatcher::templateUsesClipboard(job.replacementW);
    std::wstring originalClipText;
    bool originalClipWasSensitive = false;
    bool capturedOriginalClip = false;

    if (needsClipboardMacro) {
        capturedOriginalClip = readClipboardPlainText(originalClipText, originalClipWasSensitive);
    }

    // 3. 动态宏单遍展开（注入 8 语种本地化上下文、系统登录用户名与密码学随机 UUID v4）
    const std::wstring sysUserW = queryCurrentSystemUserNameWide();
    const std::string effLocale = resolveEffectiveLocaleCode();
    TemplateExpansionContext ctx;
    ctx.now = TextExpanderMatcher::currentLocalDateTime();
    ctx.clipboardText = capturedOriginalClip ? std::wstring_view(originalClipText) : std::wstring_view{};
    ctx.userName = sysUserW;
    ctx.localeCode = effLocale;
    ctx.uuidGenerator = []() { return generateRandomUuidV4Wide(); };

    ExpandedOutput expanded = TextExpanderMatcher::expandTemplateWithContext(job.replacementW, ctx);
    if (job.triggeredByDelimiter && job.keepDelimiter && job.delimiterChar != L'\0') {
        expanded.text.push_back(job.delimiterChar);
        if (expanded.moveLeftSteps > 0) {
            expanded.moveLeftSteps += 1;
        }
    }

    // 4. 双路径无痕投递
    if (TextExpanderMatcher::isFastDirectUnicodeCandidate(expanded.text)) {
        // 极速直注路径：0 剪贴板触碰
        sendDirectUnicodeExpansion(job.eraseCount, expanded.text, expanded.moveLeftSteps);
    } else {
        // 无痕剪贴板粘贴路径
        if (!capturedOriginalClip && job.restoreClipboard) {
            capturedOriginalClip = readClipboardPlainText(originalClipText, originalClipWasSensitive);
        }

        DWORD seqAfterWrite = 0;
        const bool wroteClip = writeClipboardPlainTextPrivate(expanded.text, seqAfterWrite);
        if (wroteClip) {
            sendBackspaces(job.eraseCount);
            Sleep(10);
            sendPasteShortcutForWindow(job.targetFgHwnd);
            if (expanded.moveLeftSteps > 0) {
                Sleep(18);
                sendLeftArrows(expanded.moveLeftSteps);
            }

            if (job.restoreClipboard && capturedOriginalClip && !originalClipWasSensitive) {
                // 可中断等待 140ms（停机时立刻唤醒退出，不卡关机）
                {
                    std::unique_lock lock(m_jobMutex);
                    m_jobCv.wait_for(
                        lock,
                        std::chrono::milliseconds(kRestoreClipboardDelayMs),
                        [this]() { return !m_workerRunning.load(std::memory_order_acquire); });
                }
                if (m_workerRunning.load(std::memory_order_acquire) &&
                    GetClipboardSequenceNumber() == seqAfterWrite) {
                    DWORD ignoredSeq = 0;
                    writeClipboardPlainTextPrivate(originalClipText, ignoredSeq);
                }
            }
        } else {
            // 剪贴板极端被占用时的降级保护：退格后逐字 Unicode 注入
            LOG_WARN("TextExpander clipboard locked during paste; falling back to Unicode input");
            sendDirectUnicodeExpansion(job.eraseCount, expanded.text, expanded.moveLeftSteps);
        }
    }

    // 5. 递增规则命中计数并持久化通知前端
    recordRuleHitAndNotify(job.ruleId);
}

void TextExpanderManager::recordRuleHitAndNotify(const std::string& ruleId) {
    if (ruleId.empty()) return;

    uint32_t nextHitCount = 0;
    bool found = false;
    {
        std::lock_guard lock(m_stateMutex);
        for (auto& r : m_rules) {
            if (r.id == ruleId) {
                ++r.hitCount;
                nextHitCount = r.hitCount;
                found = true;
                break;
            }
        }
        if (found) {
            syncMatcherRulesLocked();
            (void)saveRulesToDiskLocked();
        }
    }

    if (!found) return;

    tools3000::core::MessageBridge::instance().pushEvent("textExpander:ruleHit", {
        {"id", ruleId},
        {"hitCount", nextHitCount},
    });

    std::function<void(const std::string&, uint32_t)> cb;
    {
        std::lock_guard cbLock(m_callbackMutex);
        cb = m_onRuleExpanded;
    }
    if (cb) {
        cb(ruleId, nextHitCount);
    }
}

void TextExpanderManager::pushStateChangedEvents(bool enabled) const {
    auto& mb = tools3000::core::MessageBridge::instance();
    mb.pushEvent("textExpander:stateChanged", {{"enabled", enabled}});
    mb.pushEvent("plugins:changed", {{"id", "text_expander"}, {"enabled", enabled}});
}

std::string TextExpanderManager::generateRuleId() {
    static std::atomic<uint64_t> s_seq{1};
    const auto nowMs = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
    const uint64_t seq = s_seq.fetch_add(1, std::memory_order_relaxed);
    std::ostringstream oss;
    oss << "rule_" << nowMs << "_" << seq;
    return oss.str();
}

bool TextExpanderManager::readClipboardPlainText(std::wstring& outText, bool& outWasSensitive) {
    outText.clear();
    outWasSensitive = false;

    if (!openClipboardWithRetry()) {
        return false;
    }

    struct ClipCloser {
        ~ClipCloser() { CloseClipboard(); }
    } closer;

    const auto& fmt = PrivateClipboardFormats::get();
    if ((fmt.excludeMonitor != 0 && IsClipboardFormatAvailable(fmt.excludeMonitor)) ||
        (fmt.viewerIgnore != 0 && IsClipboardFormatAvailable(fmt.viewerIgnore))) {
        outWasSensitive = true;
    } else if (fmt.canIncludeHistory != 0 && IsClipboardFormatAvailable(fmt.canIncludeHistory)) {
        if (HANDLE hFlag = GetClipboardData(fmt.canIncludeHistory)) {
            if (const auto* pVal = static_cast<const DWORD*>(GlobalLock(hFlag))) {
                if (*pVal == 0) outWasSensitive = true;
                GlobalUnlock(hFlag);
            }
        }
    }

    if (!IsClipboardFormatAvailable(CF_UNICODETEXT)) {
        return false;
    }

    HANDLE hData = GetClipboardData(CF_UNICODETEXT);
    if (!hData) return false;

    const auto* raw = static_cast<const wchar_t*>(GlobalLock(hData));
    if (!raw) return false;
    outText.assign(raw);
    GlobalUnlock(hData);
    return true;
}

bool TextExpanderManager::writeClipboardPlainTextPrivate(const std::wstring& text, DWORD& outSeqAfterWrite) {
    outSeqAfterWrite = 0;
    if (!openClipboardWithRetry()) {
        return false;
    }

    struct ClipCloser {
        ~ClipCloser() { CloseClipboard(); }
    } closer;

    if (EmptyClipboard() == FALSE) {
        return false;
    }

    const size_t bytes = (text.size() + 1) * sizeof(wchar_t);
    HGLOBAL hMem = GlobalAlloc(GMEM_MOVEABLE, bytes);
    if (!hMem) {
        return false;
    }

    void* ptr = GlobalLock(hMem);
    if (!ptr) {
        GlobalFree(hMem);
        return false;
    }
    std::memcpy(ptr, text.c_str(), bytes);
    GlobalUnlock(hMem);

    if (!SetClipboardData(CF_UNICODETEXT, hMem)) {
        GlobalFree(hMem);
        return false;
    }

    // 写入全部三项 Windows 官方隐私排除标记，100% 防止污染剪贴板历史与 Win+V
    const auto& fmt = PrivateClipboardFormats::get();
    (void)setClipboardDwordFormat(fmt.excludeMonitor, 1u);
    (void)setClipboardDwordFormat(fmt.canIncludeHistory, 0u);
    (void)setClipboardDwordFormat(fmt.viewerIgnore, 0u);

    outSeqAfterWrite = GetClipboardSequenceNumber();
    return true;
}

void TextExpanderManager::waitModifiersReleased(DWORD maxWaitMs) {
    const ULONGLONG deadline = GetTickCount64() + maxWaitMs;
    while (GetTickCount64() < deadline) {
        const bool anyMod =
            ((GetAsyncKeyState(VK_SHIFT) & 0x8000) != 0) ||
            ((GetAsyncKeyState(VK_CONTROL) & 0x8000) != 0) ||
            ((GetAsyncKeyState(VK_MENU) & 0x8000) != 0);
        if (!anyMod) {
            return;
        }
        Sleep(10);
    }

    // 若超时仍按住 Shift（如用户敲大写触发词未及时抬手），发送防误触 KeyUp 保护
    std::vector<INPUT> release;
    if ((GetAsyncKeyState(VK_LSHIFT) & 0x8000) != 0) release.push_back(makeVkInput(VK_LSHIFT, true));
    if ((GetAsyncKeyState(VK_RSHIFT) & 0x8000) != 0) release.push_back(makeVkInput(VK_RSHIFT, true));
    if (!release.empty()) {
        SendInput(static_cast<UINT>(release.size()), release.data(), sizeof(INPUT));
    }
}

void TextExpanderManager::sendBackspaces(size_t count) {
    if (count == 0) return;
    std::vector<INPUT> inputs;
    inputs.reserve(count * 2);
    for (size_t i = 0; i < count; ++i) {
        inputs.push_back(makeVkInput(VK_BACK, false));
        inputs.push_back(makeVkInput(VK_BACK, true));
    }
    SendInput(static_cast<UINT>(inputs.size()), inputs.data(), sizeof(INPUT));
}

void TextExpanderManager::sendDirectUnicodeExpansion(
    size_t backspaces,
    std::wstring_view text,
    size_t moveLeftSteps) {
    std::vector<INPUT> inputs;
    inputs.reserve((backspaces + text.size() + moveLeftSteps) * 2);

    for (size_t i = 0; i < backspaces; ++i) {
        inputs.push_back(makeVkInput(VK_BACK, false));
        inputs.push_back(makeVkInput(VK_BACK, true));
    }
    for (wchar_t ch : text) {
        if (ch == L'\r') continue;
        if (ch == L'\n') {
            inputs.push_back(makeVkInput(VK_RETURN, false));
            inputs.push_back(makeVkInput(VK_RETURN, true));
        } else if (ch == L'\t') {
            inputs.push_back(makeVkInput(VK_TAB, false));
            inputs.push_back(makeVkInput(VK_TAB, true));
        } else {
            inputs.push_back(makeUnicodeInput(ch, false));
            inputs.push_back(makeUnicodeInput(ch, true));
        }
    }
    for (size_t i = 0; i < moveLeftSteps; ++i) {
        inputs.push_back(makeVkInput(VK_LEFT, false, true));
        inputs.push_back(makeVkInput(VK_LEFT, true, true));
    }

    if (!inputs.empty()) {
        SendInput(static_cast<UINT>(inputs.size()), inputs.data(), sizeof(INPUT));
    }
}

void TextExpanderManager::sendPasteShortcutForWindow(HWND targetFgHwnd) {
    using tools3000::clipboard::TerminalPasteKind;
    TerminalPasteKind termKind = TerminalPasteKind::NotTerminal;
    HWND hwnd = (targetFgHwnd && IsWindow(targetFgHwnd)) ? targetFgHwnd : GetForegroundWindow();
    if (hwnd && IsWindow(hwnd)) {
        HWND root = GetAncestor(hwnd, GA_ROOT);
        if (!root) root = hwnd;
        wchar_t cls[96]{};
        const int clsLen = GetClassNameW(root, cls, static_cast<int>(std::size(cls)));
        const std::wstring_view clsView(cls, clsLen > 0 ? static_cast<size_t>(clsLen) : 0u);
        const std::wstring procPath = tools3000::core::WinUtils::getProcessPathFromWindow(root);
        std::wstring exeLower;
        if (!procPath.empty()) {
            exeLower = tools3000::core::WinUtils::toLower(std::filesystem::path(procPath).filename().wstring());
        }
        termKind = tools3000::clipboard::classifyTerminalPasteTarget(clsView, exeLower);
    }

    if (termKind == TerminalPasteKind::ModernCtrlShiftV) {
        INPUT inputs[6]{};
        inputs[0] = makeVkInput(VK_CONTROL, false);
        inputs[1] = makeVkInput(VK_SHIFT, false);
        inputs[2] = makeVkInput('V', false);
        inputs[3] = makeVkInput('V', true);
        inputs[4] = makeVkInput(VK_SHIFT, true);
        inputs[5] = makeVkInput(VK_CONTROL, true);
        SendInput(6, inputs, sizeof(INPUT));
        return;
    }
    if (termKind == TerminalPasteKind::ShiftInsert) {
        INPUT inputs[4]{};
        inputs[0] = makeVkInput(VK_SHIFT, false);
        inputs[1] = makeVkInput(VK_INSERT, false, true);
        inputs[2] = makeVkInput(VK_INSERT, true, true);
        inputs[3] = makeVkInput(VK_SHIFT, true);
        SendInput(4, inputs, sizeof(INPUT));
        return;
    }

    INPUT inputs[4]{};
    inputs[0] = makeVkInput(VK_CONTROL, false);
    inputs[1] = makeVkInput('V', false);
    inputs[2] = makeVkInput('V', true);
    inputs[3] = makeVkInput(VK_CONTROL, true);
    SendInput(4, inputs, sizeof(INPUT));
}

void TextExpanderManager::sendLeftArrows(size_t count) {
    if (count == 0) return;
    std::vector<INPUT> inputs;
    inputs.reserve(count * 2);
    for (size_t i = 0; i < count; ++i) {
        inputs.push_back(makeVkInput(VK_LEFT, false, true));
        inputs.push_back(makeVkInput(VK_LEFT, true, true));
    }
    SendInput(static_cast<UINT>(inputs.size()), inputs.data(), sizeof(INPUT));
}

}  // namespace tools3000::text_expander
