// ─────────────────────────────────────────────────────────────────────────────
// WindowKitController.cpp — 纯 Rust 独立窗口管理引擎宿主控制器实现
// ─────────────────────────────────────────────────────────────────────────────

#include "core/ipc/WindowKitController.h"

#include "core/config/ConfigManager.h"
#include "core/events/MainThreadDispatcher.h"
#include "core/hotkey/HotkeyManager.h"
#include "core/ipc/MessageBridge.h"
#include "core/logger/Logger.h"
#include "core/utils/TraceId.h"
#include "core/utils/WinUtils.h"

#include <psapi.h>

#include <algorithm>
#include <fstream>
#include <sstream>

namespace {

constexpr DWORD kGracefulShutdownTimeoutMs = 1500;
constexpr int kCrashWindowSeconds = 30;
constexpr size_t kMaxCrashesInWindow = 2;

const char* const kWindowKitHotkeyNames[] = {
    "WindowKit: Toggle Always on Top",
    "WindowKit: Move Next Zone",
    "WindowKit: Move Prev Zone",
    "WindowKit: Move Next Monitor",
    "WindowKit: Rescue Offscreen",
    "WindowKit: Minimize To Tray",
    "WindowKit: Opacity Up",
    "WindowKit: Opacity Down",
    "WindowKit: Opacity Reset",
    "WindowKit: Open Layout Editor",
};

std::string formatGuidBytesLower(const uint8_t* b) {
    if (!b) return {};
    const uint32_t d1 = static_cast<uint32_t>(b[0]) | (static_cast<uint32_t>(b[1]) << 8) |
                        (static_cast<uint32_t>(b[2]) << 16) | (static_cast<uint32_t>(b[3]) << 24);
    const uint16_t d2 = static_cast<uint16_t>(b[4]) | (static_cast<uint16_t>(b[5]) << 8);
    const uint16_t d3 = static_cast<uint16_t>(b[6]) | (static_cast<uint16_t>(b[7]) << 8);
    if (d1 == 0 && d2 == 0 && d3 == 0 && b[8] == 0 && b[9] == 0 && b[10] == 0 && b[11] == 0 &&
        b[12] == 0 && b[13] == 0 && b[14] == 0 && b[15] == 0) {
        return {};
    }
    char buf[40]{};
    std::snprintf(
        buf,
        sizeof(buf),
        "%08x-%04x-%04x-%02x%02x-%02x%02x%02x%02x%02x%02x",
        d1, d2, d3,
        b[8], b[9], b[10], b[11], b[12], b[13], b[14], b[15]);
    return std::string(buf);
}

std::string queryVirtualDesktopCustomName(const std::string& guidLower) {
    if (guidLower.empty()) return {};
    const std::wstring keyPath =
        L"Software\\Microsoft\\Windows\\CurrentVersion\\Explorer\\VirtualDesktops\\Desktops\\{" +
        tools3000::core::WinUtils::utf8ToWstring(guidLower) + L"}";
    HKEY hKey = nullptr;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, keyPath.c_str(), 0, KEY_READ, &hKey) != ERROR_SUCCESS) {
        return {};
    }
    DWORD type = 0;
    wchar_t buf[256]{};
    DWORD cbData = sizeof(buf);
    const LONG rc = RegQueryValueExW(
        hKey, L"Name", nullptr, &type, reinterpret_cast<LPBYTE>(buf), &cbData);
    RegCloseKey(hKey);
    if (rc != ERROR_SUCCESS || type != REG_SZ || cbData < sizeof(wchar_t)) {
        return {};
    }
    buf[255] = L'\0';
    return tools3000::core::WinUtils::wstringToUtf8(buf);
}

std::string resolveEffectiveUiLangShort() {
    const std::string configured =
        tools3000::core::ConfigManager::instance().get<std::string>("/general/language", "auto");
    if (configured.rfind("en", 0) == 0) {
        return "en";
    }
    if (configured.rfind("zh", 0) == 0) {
        return "zh";
    }
    const LANGID langId = GetUserDefaultUILanguage();
    return (PRIMARYLANGID(langId) == LANG_CHINESE) ? "zh" : "en";
}

bool hasReparsePoint(const std::filesystem::path& p) noexcept {
    if (p.empty()) return false;
    const DWORD attrs = GetFileAttributesW(p.c_str());
    return attrs != INVALID_FILE_ATTRIBUTES && (attrs & FILE_ATTRIBUTE_REPARSE_POINT) != 0;
}

bool isHostSignedForElevatedGate() noexcept {
    static const bool s_hostSigned = []() {
        const auto exePath = tools3000::core::WinUtils::getExePath();
        return !exePath.empty() && tools3000::core::WinUtils::hasValidAuthenticodeSignature(exePath);
    }();
    return s_hostSigned;
}

std::string readFileUtf8(const std::filesystem::path& p) {
    std::ifstream ifs(p, std::ios::binary);
    if (!ifs.is_open()) return {};
    std::ostringstream oss;
    oss << ifs.rdbuf();
    return oss.str();
}

nlohmann::json buildBuiltinZoneLayouts() {
    return nlohmann::json::array({
        {
            {"id", "builtin-columns-2"},
            {"name", "2 Columns"},
            {"type", "grid"},
            {"builtin", true},
            {"rows", 1},
            {"columns", 2},
            {"rowPercents", {100}},
            {"colPercents", {50, 50}},
            {"cellMap", {{0, 1}}}
        },
        {
            {"id", "builtin-priority-3"},
            {"name", "Priority Grid"},
            {"type", "grid"},
            {"builtin", true},
            {"rows", 1},
            {"columns", 3},
            {"rowPercents", {100}},
            {"colPercents", {25, 50, 25}},
            {"cellMap", {{0, 1, 2}}}
        },
        {
            {"id", "builtin-grid-2x2"},
            {"name", "2x2 Grid"},
            {"type", "grid"},
            {"builtin", true},
            {"rows", 2},
            {"columns", 2},
            {"rowPercents", {50, 50}},
            {"colPercents", {50, 50}},
            {"cellMap", {{0, 1}, {2, 3}}}
        },
        {
            {"id", "builtin-focus-split"},
            {"name", "Main + Stack"},
            {"type", "grid"},
            {"builtin", true},
            {"rows", 2},
            {"columns", 2},
            {"rowPercents", {50, 50}},
            {"colPercents", {60, 40}},
            {"cellMap", {{0, 1}, {0, 2}}}
        }
    });
}

void deepMergeJson(nlohmann::json& target, const nlohmann::json& patch) {
    if (!target.is_object() || !patch.is_object()) {
        target = patch;
        return;
    }
    for (const auto& [k, v] : patch.items()) {
        if (v.is_object() && target.contains(k) && target[k].is_object() &&
            k != "monitorBindings") {
            deepMergeJson(target[k], v);
        } else {
            target[k] = v;
        }
    }
}

/// 将前端 SnapZoneLayout 转换为 Rust config.rs 精确识别的布局 JSON（无缝兼容 Grid & Canvas）
nlohmann::json translateLayoutToRust(const nlohmann::json& uiLayout) {
    if (!uiLayout.is_object()) return nlohmann::json::object();
    nlohmann::json out = nlohmann::json::object();
    out["id"] = uiLayout.value("id", std::string("custom-layout"));
    out["name"] = uiLayout.value("name", out["id"].get<std::string>());
    const std::string kind = uiLayout.value("type", std::string("grid"));
    out["type"] = kind;
    out["builtIn"] = uiLayout.value("builtin", uiLayout.value("builtIn", false));
    if (uiLayout.contains("gap") && uiLayout["gap"].is_number_integer()) {
        out["gap"] = uiLayout["gap"];
    }

    if (kind == "canvas") {
        nlohmann::json zones = nlohmann::json::array();
        if (uiLayout.contains("zones") && uiLayout["zones"].is_array()) {
            size_t seq = 0;
            for (const auto& z : uiLayout["zones"]) {
                if (!z.is_object()) continue;
                double x = z.value("x", 0.0);
                double y = z.value("y", 0.0);
                double w = z.value("w", z.value("width", 0.5));
                double h = z.value("h", z.value("height", 1.0));
                // 兼容 0..100 百分比或 0..1 归一化坐标
                if (w > 1.0 || h > 1.0 || x > 1.0 || y > 1.0) {
                    x /= 100.0;
                    y /= 100.0;
                    w /= 100.0;
                    h /= 100.0;
                }
                zones.push_back({
                    {"index", z.value("id", z.value("index", static_cast<int>(seq)))},
                    {"x", std::clamp(x, 0.0, 0.99)},
                    {"y", std::clamp(y, 0.0, 0.99)},
                    {"width", std::clamp(w, 0.01, 1.0)},
                    {"height", std::clamp(h, 0.01, 1.0)}
                });
                ++seq;
            }
        }
        out["zones"] = std::move(zones);
    } else {
        out["rows"] = uiLayout.value("rows", 1);
        out["columns"] = uiLayout.value("columns", 2);
        if (uiLayout.contains("rowPercents")) out["rowWeights"] = uiLayout["rowPercents"];
        else if (uiLayout.contains("rowWeights")) out["rowWeights"] = uiLayout["rowWeights"];
        if (uiLayout.contains("colPercents")) out["colWeights"] = uiLayout["colPercents"];
        else if (uiLayout.contains("colWeights")) out["colWeights"] = uiLayout["colWeights"];
        if (uiLayout.contains("cellMap")) out["cellChildMap"] = uiLayout["cellMap"];
        else if (uiLayout.contains("cellChildMap")) out["cellChildMap"] = uiLayout["cellChildMap"];
    }
    return out;
}

/// 将 Rust 导入的 PowerToys 布局转换为前端 ZoneLayoutEditor 精确识别的 SnapZoneLayout
nlohmann::json translateRustLayoutToUi(const nlohmann::json& rustLayout) {
    if (!rustLayout.is_object()) return nlohmann::json::object();
    const std::string kind = rustLayout.value("type", std::string("grid"));
    nlohmann::json out = {
        {"id", rustLayout.value("id", std::string("imported"))},
        {"name", rustLayout.value("name", std::string("Imported Layout"))},
        {"type", kind},
        {"builtin", rustLayout.value("builtIn", false)}
    };
    if (kind == "canvas") {
        nlohmann::json zones = nlohmann::json::array();
        if (rustLayout.contains("zones") && rustLayout["zones"].is_array()) {
            int seq = 0;
            for (const auto& z : rustLayout["zones"]) {
                if (!z.is_object()) continue;
                double x = z.value("x", 0.0);
                double y = z.value("y", 0.0);
                double w = z.value("width", z.value("w", 0.5));
                double h = z.value("height", z.value("h", 1.0));
                if (w <= 1.0 && h <= 1.0) {
                    x *= 100.0;
                    y *= 100.0;
                    w *= 100.0;
                    h *= 100.0;
                }
                zones.push_back({
                    {"id", z.value("index", seq)},
                    {"x", x},
                    {"y", y},
                    {"w", w},
                    {"h", h}
                });
                ++seq;
            }
        }
        out["zones"] = std::move(zones);
    } else {
        out["rows"] = rustLayout.value("rows", 1);
        out["columns"] = rustLayout.value("columns", 2);
        auto normalizeTo100 = [](const nlohmann::json& arr, int count) -> nlohmann::json {
            nlohmann::json res = nlohmann::json::array();
            if (arr.is_array() && !arr.empty()) {
                double sum = 0.0;
                for (const auto& v : arr) {
                    if (v.is_number() && v.get<double>() > 0.0) sum += v.get<double>();
                }
                if (sum > 0.0) {
                    for (const auto& v : arr) {
                        const double n = v.is_number() ? std::max(1.0, v.get<double>()) : 1.0;
                        res.push_back(std::round((n / sum) * 1000.0) / 10.0);
                    }
                    return res;
                }
            }
            const int n = std::max(1, count);
            for (int i = 0; i < n; ++i) res.push_back(std::round((1000.0 / n)) / 10.0);
            return res;
        };
        out["rowPercents"] = normalizeTo100(rustLayout.value("rowWeights", nlohmann::json::array()), out["rows"].get<int>());
        out["colPercents"] = normalizeTo100(rustLayout.value("colWeights", nlohmann::json::array()), out["columns"].get<int>());
        if (rustLayout.contains("cellChildMap")) {
            out["cellMap"] = rustLayout["cellChildMap"];
        }
    }
    return out;
}

std::string resolveAccentHex(const std::string& accentName) {
    if (accentName.size() == 7 && accentName[0] == '#') return accentName;
    if (accentName == "violet") return "#8B5CF6";
    if (accentName == "cyan") return "#06B6D4";
    if (accentName == "amber") return "#F59E0B";
    if (accentName == "mint") return "#10B981";
    if (accentName == "coral") return "#F43F5E";
    return "#3B82F6";
}

}  // namespace

namespace tools3000::core {

WindowKitController& WindowKitController::instance() {
    static WindowKitController inst;
    return inst;
}

WindowKitController::~WindowKitController() {
    shutdown();
}

bool WindowKitController::isRunningDesired() const noexcept {
    return m_shouldRun.load(std::memory_order_acquire);
}

nlohmann::json WindowKitController::buildDefaultSettingsJson() {
    return {
        {"features", {
            {"snapZones", false},
            {"workspaces", false},
            {"quickSwitch", false},
            {"alwaysOnTop", false},
            {"opacity", false},
            {"altDrag", false},
            {"focusBorder", false},
            {"multiMonitor", false}
        }},
        {"snapZones", {
            {"triggerMode", "shift"},
            {"spanMultipleZones", true},
            {"showZoneNumbers", true},
            {"zoneGap", 8},
            {"overlayOpacity", 70},
            {"rememberLastZone", true},
            {"perVirtualDesktop", false},
            {"excludedApps", nlohmann::json::array()},
            {"layouts", buildBuiltinZoneLayouts()},
            {"monitorBindings", nlohmann::json::object()}
        }},
        {"workspaces", {
            {"minimizeUnrelated", false},
            {"items", nlohmann::json::array()}
        }},
        {"quickSwitch", {
            {"currentMonitorOnly", false},
            {"showPreview", true},
            {"showNumbers", true},
            {"density", "comfortable"},
            {"enablePinyinSearch", true}
        }},
        {"alwaysOnTop", {
            {"showBorder", true},
            {"borderThickness", 2},
            {"borderColor", "#0ea5e9"}
        }},
        {"opacity", {
            {"stepPercent", 10},
            {"minPercent", 25},
            {"wheelOnTitlebar", true}
        }},
        {"altDrag", {
            {"modifier", "alt"},
            {"snapToZones", true},
            {"excludedApps", nlohmann::json::array({
                "blender.exe", "photoshop.exe", "acad.exe", "revit.exe", "maya.exe",
                "3dsmax.exe", "houdini.exe", "mstsc.exe", "vmware.exe", "cs2.exe", "dota2.exe"
            })}
        }},
        {"focusBorder", {
            {"thickness", 2},
            {"color", "#8b5cf6"},
            {"cornerRadius", 10}
        }},
        {"multiMonitor", {
            {"centerNewWindows", false},
            {"enableTrayMinimize", true}
        }},
        {"hotkeys", {
            {"toggleAlwaysOnTop", "Ctrl+Win+T"},
            {"moveNextZone", "Win+Alt+Right"},
            {"movePrevZone", "Win+Alt+Left"},
            {"moveNextMonitor", "Ctrl+Win+Right"},
            {"rescueOffscreen", "Ctrl+Win+G"},
            {"minimizeToTray", "Ctrl+Win+Down"},
            {"opacityUp", "Ctrl+Win+Up"},
            {"opacityDown", "Ctrl+Win+PageDown"},
            {"opacityReset", "Ctrl+Win+0"},
            {"openLayoutEditor", "Win+Shift+~"}
        }}
    };
}

bool WindowKitController::hasAnyActiveFeature(bool masterEnabled, const nlohmann::json& settings) noexcept {
    try {
        if (!masterEnabled || !settings.is_object()) {
            return false;
        }
        const auto it = settings.find("features");
        if (it == settings.end() || !it->is_object()) {
            return false;
        }
        for (const auto& [_, val] : it->items()) {
            if (val.is_boolean() && val.get<bool>()) {
                return true;
            }
        }
    } catch (...) {
    }
    return false;
}

WPARAM WindowKitController::resolveActionCode(const std::string& actionName) noexcept {
    if (actionName == "toggle_always_on_top" || actionName == "toggleAlwaysOnTop") return kWindowKitActionToggleAlwaysOnTop;
    if (actionName == "opacity_up" || actionName == "opacityUp") return kWindowKitActionOpacityUp;
    if (actionName == "opacity_down" || actionName == "opacityDown") return kWindowKitActionOpacityDown;
    if (actionName == "opacity_reset" || actionName == "opacityReset") return kWindowKitActionOpacityReset;
    if (actionName == "move_next_zone" || actionName == "moveNextZone") return kWindowKitActionMoveNextZone;
    if (actionName == "move_prev_zone" || actionName == "movePrevZone") return kWindowKitActionMovePrevZone;
    if (actionName == "move_zone_index" || actionName == "snapForegroundToZone") return kWindowKitActionMoveZoneIndex;
    if (actionName == "restore_from_zone" || actionName == "restoreFromZone") return kWindowKitActionRestoreFromZone;
    if (actionName == "rescue_offscreen" || actionName == "gatherOffscreen") return kWindowKitActionRescueOffscreen;
    if (actionName == "move_next_monitor" || actionName == "moveNextMonitor") return kWindowKitActionMoveNextMonitor;
    if (actionName == "minimize_to_tray" || actionName == "minimizeToTray") return kWindowKitActionMinimizeToTray;
    if (actionName == "restore_all_tray" || actionName == "restoreAllTray") return kWindowKitActionRestoreAllTray;
    if (actionName == "open_quick_switch" || actionName == "showQuickSwitch") return kWindowKitActionOpenQuickSwitch;
    if (actionName == "capture_workspace" || actionName == "captureWorkspace") return kWindowKitActionCaptureWorkspace;
    if (actionName == "launch_workspace" || actionName == "launchWorkspace") return kWindowKitActionLaunchWorkspace;
    if (actionName == "import_powertoys_zones") return kWindowKitActionImportPowerToysZones;
    if (actionName == "query_monitors") return kWindowKitActionQueryMonitors;
    if (actionName == "query_runtime_stats") return kWindowKitActionQueryRuntimeStats;
    if (actionName == "show_zones_preview" || actionName == "showZoneOverlayPreview") return kWindowKitActionShowZonesPreview;
    if (actionName == "open_layout_editor" || actionName == "openLayoutEditor") return kWindowKitActionOpenLayoutEditor;
    return 0;
}

std::filesystem::path WindowKitController::resolveBinaryPath() const {
    return WinUtils::getExeDirectory() / kWindowKitExeName;
}

std::filesystem::path WindowKitController::resolveRuntimeConfigPath() const {
    return WinUtils::getConfigDirectory() / L"window_kit_runtime.json";
}

std::filesystem::path WindowKitController::resolveActionPayloadPath() const {
    return WinUtils::getConfigDirectory() / L"window_kit_action.json";
}

std::filesystem::path WindowKitController::resolveEditorResultPath() const {
    return WinUtils::getConfigDirectory() / L"window_kit_layout_editor_result.json";
}

bool WindowKitController::isBinaryInstalled() const {
    const auto bin = resolveBinaryPath();
    std::error_code ec;
    if (!std::filesystem::is_regular_file(bin, ec) || ec) {
        return false;
    }
    if (hasReparsePoint(bin.parent_path()) || hasReparsePoint(bin)) {
        return false;
    }
    // 提权且正式签名的宿主强制校验独立子进程 Authenticode 签名（未签名本地开发构建不受影响）
    if (WinUtils::isProcessElevated() && isHostSignedForElevatedGate()) {
        if (!WinUtils::hasValidAuthenticodeSignature(bin)) {
            LOG_ERROR("[WindowKit] Rejected unsigned Tools3000_WindowKit.exe in elevated signed host");
            return false;
        }
    }
    return true;
}

void WindowKitController::loadFromConfigLocked() {
    m_settings = buildDefaultSettingsJson();
    m_masterEnabled = false;

    auto& cfg = ConfigManager::instance();
    const auto saved = cfg.get<nlohmann::json>("/windowManager", nlohmann::json::object());
    if (saved.is_object()) {
        if (saved.contains("enabled") && saved["enabled"].is_boolean()) {
            m_masterEnabled = saved["enabled"].get<bool>();
        }
        if (saved.contains("settings") && saved["settings"].is_object()) {
            deepMergeJson(m_settings, saved["settings"]);
        } else {
            // 兼容扁平结构
            for (const auto& [k, v] : saved.items()) {
                if (k == "enabled") continue;
                if (m_settings.contains(k) && m_settings[k].is_object() && v.is_object()) {
                    deepMergeJson(m_settings[k], v);
                }
            }
        }
    }
    if (cfg.has("/plugins/window_manager/enabled")) {
        m_masterEnabled = cfg.get<bool>("/plugins/window_manager/enabled", m_masterEnabled);
    }

    if (!m_settings.contains("snapZones") || !m_settings["snapZones"].is_object()) {
        m_settings["snapZones"] = buildDefaultSettingsJson()["snapZones"];
    }
    if (!m_settings["snapZones"].contains("layouts") ||
        !m_settings["snapZones"]["layouts"].is_array() ||
        m_settings["snapZones"]["layouts"].empty()) {
        m_settings["snapZones"]["layouts"] = buildBuiltinZoneLayouts();
    }
}

void WindowKitController::saveToConfigLocked() const {
    auto& cfg = ConfigManager::instance();
    nlohmann::json patch = nlohmann::json::object();
    patch["windowManager"]["enabled"] = m_masterEnabled;
    patch["windowManager"]["settings"] = m_settings;
    patch["plugins"]["window_manager"]["enabled"] = m_masterEnabled;
    cfg.mergePatch(patch, "/windowManager");
}

nlohmann::json WindowKitController::buildRuntimeStatsLocked() const {
    const bool installed = isBinaryInstalled();
    const bool running = isProcessAliveLocked();

    nlohmann::json activeFeatures = nlohmann::json::array();
    if (m_settings.contains("features") && m_settings["features"].is_object()) {
        for (const auto& [k, v] : m_settings["features"].items()) {
            if (v.is_boolean() && v.get<bool>()) {
                activeFeatures.push_back(k);
            }
        }
    }

    if (!running || !m_processHandle) {
        return {
            {"installed", installed},
            {"running", false},
            {"pid", 0},
            {"workingSetBytes", 0},
            {"privateBytes", 0},
            {"handleCount", 0},
            {"gdiObjects", 0},
            {"userObjects", 0},
            {"pinnedCount", 0},
            {"trayMinimizedCount", 0},
            {"activeFeatures", activeFeatures},
            {"restartCount", m_restartCount},
            {"circuitBreakerTripped", m_circuitBreakerTripped}
        };
    }

    PROCESS_MEMORY_COUNTERS_EX pmc{};
    pmc.cb = sizeof(pmc);
    GetProcessMemoryInfo(m_processHandle, reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&pmc), sizeof(pmc));

    DWORD handles = 0;
    GetProcessHandleCount(m_processHandle, &handles);
    const DWORD gdi = GetGuiResources(m_processHandle, GR_GDIOBJECTS);
    const DWORD user = GetGuiResources(m_processHandle, GR_USEROBJECTS);

    return {
        {"installed", installed},
        {"running", true},
        {"pid", static_cast<uint32_t>(m_processId)},
        {"workingSetBytes", static_cast<uint64_t>(pmc.WorkingSetSize)},
        {"privateBytes", static_cast<uint64_t>(pmc.PrivateUsage)},
        {"handleCount", static_cast<uint32_t>(handles)},
        {"gdiObjects", static_cast<uint32_t>(gdi)},
        {"userObjects", static_cast<uint32_t>(user)},
        {"pinnedCount", 0},
        {"trayMinimizedCount", 0},
        {"activeFeatures", activeFeatures},
        {"restartCount", m_restartCount},
        {"circuitBreakerTripped", m_circuitBreakerTripped}
    };
}

nlohmann::json WindowKitController::buildResponseJsonLocked() const {
    const auto stats = buildRuntimeStatsLocked();
    return {
        {"enabled", m_masterEnabled},
        {"running", stats.value("running", false)},
        {"exeAvailable", stats.value("installed", false)},
        {"circuitBreakerTripped", m_circuitBreakerTripped},
        {"runtimeStats", stats},
        {"settings", m_settings.is_object() && !m_settings.empty() ? m_settings : buildDefaultSettingsJson()}
    };
}

nlohmann::json WindowKitController::buildRustRuntimePayloadLocked(const std::string& traceId) const {
    auto& cfg = ConfigManager::instance();
    const std::string theme = cfg.get<std::string>("/general/theme", "dark");
    const std::string accentColor = resolveAccentHex(cfg.get<std::string>("/general/accentColor", "blue"));
    const std::string logLevel = cfg.get<std::string>("/general/logLevel", Logger::kDefaultLogLevel);

    const auto& s = m_settings;
    const auto features = s.value("features", nlohmann::json::object());
    const auto sz = s.value("snapZones", nlohmann::json::object());
    const auto qs = s.value("quickSwitch", nlohmann::json::object());
    const auto aot = s.value("alwaysOnTop", nlohmann::json::object());
    const auto op = s.value("opacity", nlohmann::json::object());
    const auto ad = s.value("altDrag", nlohmann::json::object());
    const auto fb = s.value("focusBorder", nlohmann::json::object());
    const auto mm = s.value("multiMonitor", nlohmann::json::object());
    const auto ws = s.value("workspaces", nlohmann::json::object());

    // 严格按模块物理隔离排除名单：snapZones.excludedApps 与 altDrag.excludedApps 独立下发，
    // 绝不混入顶层 excludedApps 误伤 QuickSwitch / Workspaces / MultiMonitor
    auto sanitizeExcluded = [](const nlohmann::json& arr) -> nlohmann::json {
        nlohmann::json out = nlohmann::json::array();
        if (!arr.is_array()) return out;
        for (const auto& item : arr) {
            if (item.is_string() && !item.get<std::string>().empty()) {
                out.push_back(item.get<std::string>());
            }
        }
        return out;
    };
    const nlohmann::json globalExcluded = nlohmann::json::array();
    const nlohmann::json snapExcluded = sanitizeExcluded(sz.value("excludedApps", nlohmann::json::array()));
    const nlohmann::json altDragExcluded = sanitizeExcluded(ad.value("excludedApps", nlohmann::json::array()));

    // 转换 SnapZones 布局列表为 Rust 精确格式
    nlohmann::json rustLayouts = nlohmann::json::array();
    std::string firstLayoutId = "builtin-columns-2";
    if (sz.contains("layouts") && sz["layouts"].is_array()) {
        for (const auto& l : sz["layouts"]) {
            auto translated = translateLayoutToRust(l);
            if (translated.is_object() && !translated.value("id", std::string{}).empty()) {
                if (rustLayouts.empty()) firstLayoutId = translated["id"].get<std::string>();
                rustLayouts.push_back(std::move(translated));
            }
        }
    }

    std::string triggerMode = sz.value("triggerMode", std::string("shift"));
    if (triggerMode == "edge") triggerMode = "top_edge";

    const double overlayOpacityPct = sz.value("overlayOpacity", 70.0);
    const double highlightOpacity = std::clamp(overlayOpacityPct / 100.0 * 0.55, 0.10, 0.80);

    return {
        {"schemaVersion", 2},
        {"traceId", traceId},
        {"enabled", m_masterEnabled},
        {"theme", theme},
        {"accentColor", accentColor},
        {"logDir", WinUtils::pathToUtf8(WinUtils::getLogDirectory())},
        {"logLevel", logLevel},
        {"excludedApps", globalExcluded},
        {"features", features},
        {"quickSwitch", {
            {"showLivePreview", qs.value("showPreview", true)},
            {"showAllMonitors", !qs.value("currentMonitorOnly", false)},
            {"showProcessBadge", true},
            {"showNumbers", qs.value("showNumbers", true)},
            {"density", qs.value("density", std::string("comfortable"))},
            {"enablePinyinSearch", qs.value("enablePinyinSearch", true)},
            {"includeMinimized", true}
        }},
        {"snapZones", {
            {"triggerMode", triggerMode},
            {"zoneGap", sz.value("zoneGap", 8)},
            {"highlightOpacity", highlightOpacity},
            {"showZoneNumbers", sz.value("showZoneNumbers", true)},
            {"allowMultiZoneSpan", sz.value("spanMultipleZones", true)},
            {"rememberLastZone", sz.value("rememberLastZone", true)},
            {"restoreOriginalSize", sz.value("restoreOriginalSize", true)},
            {"perVirtualDesktop", sz.value("perVirtualDesktop", false)},
            {"excludedApps", snapExcluded},
            {"activeLayoutId", firstLayoutId},
            {"monitorBindings", sz.value("monitorBindings", nlohmann::json::object())},
            {"layouts", rustLayouts}
        }},
        {"alwaysOnTop", {
            {"showBorder", aot.value("showBorder", true)},
            {"borderColor", aot.value("borderColor", std::string("#0ea5e9"))},
            {"borderWidth", aot.value("borderThickness", 2)}
        }},
        {"opacity", {
            {"stepPercent", op.value("stepPercent", 10)},
            {"minPercent", op.value("minPercent", 25)},
            {"altWheelEnabled", op.value("wheelOnTitlebar", true)}
        }},
        {"altDrag", {
            {"bringToFront", true},
            {"modifier", ad.value("modifier", std::string("alt"))},
            {"snapToZones", ad.value("snapToZones", true)},
            {"excludedApps", altDragExcluded}
        }},
        {"focusBorder", {
            {"borderColor", fb.value("color", std::string("#8b5cf6"))},
            {"borderWidth", fb.value("thickness", 2)},
            {"cornerRadius", fb.value("cornerRadius", 10)},
            {"opacity", 0.90}
        }},
        {"multiMonitor", {
            {"keepProportionalSize", true},
            {"centerNewWindows", mm.value("centerNewWindows", false)},
            {"enableTrayMinimize", mm.value("enableTrayMinimize", true)}
        }},
        {"workspaces", {
            {"minimizeUnrelated", ws.value("minimizeUnrelated", false)},
            {"items", ws.value("items", nlohmann::json::array())}
        }}
    };
}

bool WindowKitController::writeRuntimeConfigAtomicLocked(const std::string& traceId) const {
    const auto cfgDir = WinUtils::getConfigDirectory();
    std::error_code ec;
    std::filesystem::create_directories(cfgDir, ec);
    const auto target = resolveRuntimeConfigPath();
    const std::string serialized =
        buildRustRuntimePayloadLocked(traceId).dump(2, ' ', false, nlohmann::json::error_handler_t::replace);
    return WinUtils::atomicWriteFileWithFlush(target.wstring(), serialized);
}

bool WindowKitController::isProcessAliveLocked() const noexcept {
    if (!m_processHandle) return false;
    DWORD exitCode = 0;
    if (!GetExitCodeProcess(m_processHandle, &exitCode)) {
        return false;
    }
    return exitCode == STILL_ACTIVE;
}

HWND WindowKitController::findManagedControlWindowLocked() const noexcept {
    if (m_processId == 0) return nullptr;
    HWND cursor = nullptr;
    for (;;) {
        cursor = FindWindowExW(nullptr, cursor, kWindowKitControlClassName, nullptr);
        if (!cursor) break;
        DWORD pid = 0;
        GetWindowThreadProcessId(cursor, &pid);
        if (pid == m_processId) {
            return cursor;
        }
    }
    return nullptr;
}

void WindowKitController::postControlMessageLocked(UINT msg, WPARAM wp, LPARAM lp) const noexcept {
    if (!isProcessAliveLocked()) return;
    HWND ctrl = findManagedControlWindowLocked();
    if (ctrl && IsWindow(ctrl)) {
        // 铁律：永远仅用异步 PostMessageW，绝不调用 SendMessageW 造成宿主阻塞连坐
        PostMessageW(ctrl, msg, wp, lp);
    }
}

bool WindowKitController::launchManagedProcessLocked(const std::string& traceId) {
    if (isProcessAliveLocked()) {
        return true;
    }
    if (m_processHandle) {
        CloseHandle(m_processHandle);
        m_processHandle = nullptr;
        m_processId = 0;
    }

    if (!isBinaryInstalled()) {
        LOG_WARN("[WindowKit] Binary missing or rejected, skipping launch");
        return false;
    }

    if (!writeRuntimeConfigAtomicLocked(traceId)) {
        LOG_ERROR("[WindowKit] Failed to write runtime config file");
        return false;
    }

    const auto exePath = resolveBinaryPath();
    const auto cfgPath = resolveRuntimeConfigPath();
    std::wstring cmdLine = L"\"";
    cmdLine += exePath.wstring();
    cmdLine += L"\" --managed --config \"";
    cmdLine += cfgPath.wstring();
    cmdLine += L"\" --parent-pid ";
    cmdLine += std::to_wstring(GetCurrentProcessId());

    STARTUPINFOW si{};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESHOWWINDOW;
    si.wShowWindow = SW_HIDE;

    PROCESS_INFORMATION pi{};
    const BOOL ok = CreateProcessW(
        exePath.c_str(),
        cmdLine.data(),
        nullptr,
        nullptr,
        FALSE,
        CREATE_NO_WINDOW | CREATE_UNICODE_ENVIRONMENT,
        nullptr,
        exePath.parent_path().c_str(),
        &si,
        &pi);

    if (!ok) {
        LOG_ERROR("[WindowKit] CreateProcessW failed, error={}", GetLastError());
        return false;
    }

    WinUtils::assignProcessToCurrentJob(pi.hProcess);
    CloseHandle(pi.hThread);
    m_processHandle = pi.hProcess;
    m_processId = pi.dwProcessId;
    LOG_INFO("[WindowKit] Launched managed process pid={} traceId={}", m_processId, traceId);
    return true;
}

void WindowKitController::stopManagedProcessLocked(DWORD gracefulTimeoutMs) {
    if (!m_processHandle) {
        m_processId = 0;
        return;
    }
    const DWORD pid = m_processId;
    if (isProcessAliveLocked()) {
        postControlMessageLocked(kWindowKitMsgGracefulShutdown, 0, 0);
        const DWORD waitRes = WaitForSingleObject(m_processHandle, gracefulTimeoutMs);
        if (waitRes != WAIT_OBJECT_0 && isProcessAliveLocked()) {
            LOG_WARN("[WindowKit] Graceful shutdown timed out after {}ms (pid={}); forcing termination",
                     gracefulTimeoutMs, pid);
            TerminateProcess(m_processHandle, 0);
            WaitForSingleObject(m_processHandle, 500);
        }
    }
    CloseHandle(m_processHandle);
    m_processHandle = nullptr;
    m_processId = 0;
    LOG_INFO("[WindowKit] Managed process stopped cleanly (pid={})", pid);
}

void WindowKitController::startWatchdogLocked() {
    std::lock_guard wLock(m_watchdogLifecycleMutex);
    if (m_watchdogRunning.load(std::memory_order_acquire)) {
        return;
    }
    m_watchdogRunning.store(true, std::memory_order_release);
    m_watchdogThread = std::thread(&WindowKitController::watchdogLoop, this);
}

void WindowKitController::stopWatchdog() {
    std::thread toJoin;
    {
        std::lock_guard wLock(m_watchdogLifecycleMutex);
        if (!m_watchdogRunning.exchange(false, std::memory_order_acq_rel)) {
            return;
        }
        m_watchdogCv.notify_all();
        toJoin = std::move(m_watchdogThread);
    }
    if (toJoin.joinable() && toJoin.get_id() != std::this_thread::get_id()) {
        toJoin.join();
    }
}

void WindowKitController::watchdogLoop() {
    while (m_watchdogRunning.load(std::memory_order_acquire)) {
        HANDLE waitHandle = nullptr;
        {
            std::lock_guard lock(m_mutex);
            if (!m_shouldRun.load(std::memory_order_acquire) || m_circuitBreakerTripped) {
                break;
            }
            if (m_processHandle) {
                DuplicateHandle(
                    GetCurrentProcess(),
                    m_processHandle,
                    GetCurrentProcess(),
                    &waitHandle,
                    SYNCHRONIZE,
                    FALSE,
                    0);
            }
        }

        if (!waitHandle) {
            std::unique_lock cvLock(m_watchdogCvMutex);
            m_watchdogCv.wait_for(cvLock, std::chrono::milliseconds(500), [this]() {
                return !m_watchdogRunning.load(std::memory_order_acquire);
            });
            continue;
        }

        const DWORD waitRes = WaitForSingleObject(waitHandle, 500);
        CloseHandle(waitHandle);
        if (!m_watchdogRunning.load(std::memory_order_acquire) ||
            m_shuttingDown.load(std::memory_order_acquire)) {
            break;
        }
        if (waitRes != WAIT_OBJECT_0) {
            continue;
        }

        bool shouldRestart = false;
        int backoffMs = 1000;
        {
            std::lock_guard lock(m_mutex);
            if (!m_shouldRun.load(std::memory_order_acquire) || m_shuttingDown.load(std::memory_order_acquire)) {
                break;
            }
            if (isProcessAliveLocked()) {
                continue;
            }
            const auto now = std::chrono::steady_clock::now();
            while (!m_recentCrashTimes.empty() &&
                   std::chrono::duration_cast<std::chrono::seconds>(now - m_recentCrashTimes.front()).count() >
                       kCrashWindowSeconds) {
                m_recentCrashTimes.pop_front();
            }
            m_recentCrashTimes.push_back(now);

            if (m_recentCrashTimes.size() > kMaxCrashesInWindow) {
                m_circuitBreakerTripped = true;
                if (m_processHandle) {
                    CloseHandle(m_processHandle);
                    m_processHandle = nullptr;
                    m_processId = 0;
                }
                LOG_ERROR("[WindowKit] Crash circuit breaker tripped ({} unexpected exits in {}s); staying degraded to protect host",
                          m_recentCrashTimes.size(), kCrashWindowSeconds);
            } else {
                shouldRestart = true;
                backoffMs = (m_recentCrashTimes.size() == 1) ? 1000 : 3000;
                ++m_restartCount;
                LOG_WARN("[WindowKit] Managed process exited unexpectedly; scheduling backoff restart #{} in {}ms",
                         m_restartCount, backoffMs);
            }
        }

        if (!shouldRestart) {
            pushStateChangedEvents();
            break;
        }

        {
            std::unique_lock cvLock(m_watchdogCvMutex);
            m_watchdogCv.wait_for(cvLock, std::chrono::milliseconds(backoffMs), [this]() {
                return !m_watchdogRunning.load(std::memory_order_acquire);
            });
        }
        if (!m_watchdogRunning.load(std::memory_order_acquire) ||
            m_shuttingDown.load(std::memory_order_acquire)) {
            break;
        }

        {
            std::lock_guard lock(m_mutex);
            if (m_shouldRun.load(std::memory_order_acquire) && !m_circuitBreakerTripped && !isProcessAliveLocked()) {
                launchManagedProcessLocked(TraceId::generate());
            }
        }
        pushStateChangedEvents();
    }
}

void WindowKitController::unregisterAllHotkeys() {
    auto& hm = HotkeyManager::instance();
    for (const char* name : kWindowKitHotkeyNames) {
        hm.unregisterHotkey(name);
    }
    m_registeredHotkeys.clear();
}

void WindowKitController::syncHotkeysLocked() {
    std::map<std::string, std::pair<std::string, std::string>> desired;
    if (hasAnyActiveFeature(m_masterEnabled, m_settings)) {
        const auto& features = m_settings["features"];
        const auto hotkeys = m_settings.value("hotkeys", nlohmann::json::object());

        auto collectIfEnabled = [&](const char* featureKey, const char* hkKey, const char* regName, const char* actionName) {
            if (!features.value(featureKey, false)) return;
            const std::string binding = hotkeys.value(hkKey, std::string{});
            if (binding.empty()) return;
            desired.emplace(regName, std::make_pair(binding, std::string(actionName)));
        };

        collectIfEnabled("alwaysOnTop", "toggleAlwaysOnTop", kWindowKitHotkeyNames[0], "toggle_always_on_top");
        collectIfEnabled("snapZones", "moveNextZone", kWindowKitHotkeyNames[1], "move_next_zone");
        collectIfEnabled("snapZones", "movePrevZone", kWindowKitHotkeyNames[2], "move_prev_zone");
        collectIfEnabled("multiMonitor", "moveNextMonitor", kWindowKitHotkeyNames[3], "move_next_monitor");
        collectIfEnabled("multiMonitor", "rescueOffscreen", kWindowKitHotkeyNames[4], "rescue_offscreen");
        collectIfEnabled("multiMonitor", "minimizeToTray", kWindowKitHotkeyNames[5], "minimize_to_tray");
        collectIfEnabled("opacity", "opacityUp", kWindowKitHotkeyNames[6], "opacity_up");
        collectIfEnabled("opacity", "opacityDown", kWindowKitHotkeyNames[7], "opacity_down");
        collectIfEnabled("opacity", "opacityReset", kWindowKitHotkeyNames[8], "opacity_reset");
        collectIfEnabled("snapZones", "openLayoutEditor", kWindowKitHotkeyNames[9], "open_layout_editor");
    }

    // 快照 Diff 去重：若目标热键集合与当前已注册集合 100% 一致，直接跳过，避免调滑块/切开关狂刷热键日志
    std::map<std::string, std::string> nextSnapshot;
    for (const auto& [regName, pair] : desired) {
        nextSnapshot.emplace(regName, pair.first);
    }
    if (nextSnapshot == m_registeredHotkeys) {
        return;
    }

    auto& hm = HotkeyManager::instance();
    // 1. 仅注销已移除或快捷键变更的条目
    for (const auto& [regName, oldBinding] : m_registeredHotkeys) {
        const auto it = nextSnapshot.find(regName);
        if (it == nextSnapshot.end() || it->second != oldBinding) {
            hm.unregisterHotkey(regName);
        }
    }
    // 2. 仅注册新增或快捷键变更的条目
    std::map<std::string, std::string> actualSnapshot;
    for (const auto& [regName, pair] : desired) {
        const auto oldIt = m_registeredHotkeys.find(regName);
        if (oldIt != m_registeredHotkeys.end() && oldIt->second == pair.first) {
            actualSnapshot.emplace(regName, pair.first);
            continue;
        }
        if (auto parsed = HotkeyDef::fromString(pair.first)) {
            const std::string act = pair.second;
            hm.registerHotkey(regName, *parsed, [act]() {
                WindowKitController::instance().execAction(act);
            });
            actualSnapshot.emplace(regName, pair.first);
        }
    }
    m_registeredHotkeys = std::move(actualSnapshot);
}

void WindowKitController::reconcileProcessStateLocked(bool allowResetBreaker) {
    const bool wantRun = hasAnyActiveFeature(m_masterEnabled, m_settings);
    m_shouldRun.store(wantRun, std::memory_order_release);

    if (allowResetBreaker) {
        m_circuitBreakerTripped = false;
        m_recentCrashTimes.clear();
    }

    syncHotkeysLocked();

    if (!wantRun) {
        stopManagedProcessLocked(kGracefulShutdownTimeoutMs);
        return;
    }

    if (m_circuitBreakerTripped) {
        return;
    }

    const std::string traceId = TraceId::current().empty() ? TraceId::generate() : TraceId::current();
    if (isProcessAliveLocked()) {
        if (writeRuntimeConfigAtomicLocked(traceId)) {
            postControlMessageLocked(kWindowKitMsgReloadConfig, 0, 0);
        }
    } else {
        launchManagedProcessLocked(traceId);
    }
}

void WindowKitController::initialize() {
    bool shouldWatch = false;
    {
        std::lock_guard lock(m_mutex);
        if (m_initialized) return;
        m_shuttingDown.store(false, std::memory_order_release);
        loadFromConfigLocked();
        reconcileProcessStateLocked(true);
        shouldWatch = m_shouldRun.load(std::memory_order_acquire) && isProcessAliveLocked();
        m_initialized = true;
    }
    if (shouldWatch) {
        startWatchdogLocked();
        LOG_INFO("[WindowKit] Controller initialized with active features");
    } else {
        LOG_DEBUG("[WindowKit] Controller initialized in default-off zero-footprint mode");
    }
}

void WindowKitController::shutdown() {
    m_shuttingDown.store(true, std::memory_order_release);
    m_shouldRun.store(false, std::memory_order_release);
    stopWatchdog();
    unregisterAllHotkeys();
    {
        std::lock_guard lock(m_mutex);
        stopManagedProcessLocked(kGracefulShutdownTimeoutMs);
        m_initialized = false;
    }
}

nlohmann::json WindowKitController::getSettingsJson() const {
    std::lock_guard lock(m_mutex);
    return buildResponseJsonLocked();
}

nlohmann::json WindowKitController::updateSettings(const nlohmann::json& params) {
    if (!params.is_object()) {
        return {{"success", false}, {"error", "invalid params"}};
    }

    bool shouldWatch = false;
    bool stopWatch = false;
    nlohmann::json response;
    {
        std::lock_guard lock(m_mutex);
        if (!m_initialized) {
            loadFromConfigLocked();
            m_initialized = true;
        }
        if (params.contains("enabled") && params["enabled"].is_boolean()) {
            m_masterEnabled = params["enabled"].get<bool>();
        }
        if (params.contains("settings") && params["settings"].is_object()) {
            deepMergeJson(m_settings, params["settings"]);
        } else {
            // 兼容直接扁平传入子段（如 { features: { snapZones: true } }）
            for (const auto& [k, v] : params.items()) {
                if (k == "enabled") continue;
                if (m_settings.contains(k) && m_settings[k].is_object() && v.is_object()) {
                    deepMergeJson(m_settings[k], v);
                }
            }
        }
        saveToConfigLocked();
        reconcileProcessStateLocked(true);
        shouldWatch = m_shouldRun.load(std::memory_order_acquire) && isProcessAliveLocked();
        stopWatch = !m_shouldRun.load(std::memory_order_acquire);
        response = buildResponseJsonLocked();
    }

    if (shouldWatch) {
        startWatchdogLocked();
    } else if (stopWatch) {
        stopWatchdog();
    }
    pushStateChangedEvents();
    response["success"] = true;
    return response;
}

bool WindowKitController::setMasterEnabled(bool enabled, std::string& error) {
    if (enabled && !isBinaryInstalled()) {
        error = "Tools3000_WindowKit.exe not found";
        return false;
    }
    auto res = updateSettings({{"enabled", enabled}});
    if (!res.value("success", false)) {
        error = res.value("error", std::string("failed to update window manager state"));
        return false;
    }
    return true;
}

nlohmann::json WindowKitController::execAction(const std::string& actionName, const nlohmann::json& arg) {
    const WPARAM code = resolveActionCode(actionName);
    if (code == 0) {
        return {{"success", false}, {"error", "unknown action"}};
    }

    LPARAM lParam = 0;
    std::string workspaceId;
    if (arg.is_number_integer()) {
        lParam = static_cast<LPARAM>(arg.get<int64_t>());
    } else if (arg.is_object()) {
        if (arg.contains("index") && arg["index"].is_number_integer()) {
            lParam = static_cast<LPARAM>(arg["index"].get<int64_t>());
        } else if (arg.contains("zoneIndex") && arg["zoneIndex"].is_number_integer()) {
            lParam = static_cast<LPARAM>(arg["zoneIndex"].get<int64_t>());
        } else if (arg.contains("durationMs") && arg["durationMs"].is_number_integer()) {
            lParam = static_cast<LPARAM>(arg["durationMs"].get<int64_t>());
        }
        if (arg.contains("workspaceId") && arg["workspaceId"].is_string()) {
            workspaceId = arg["workspaceId"].get<std::string>();
        }
    }

    if (code == kWindowKitActionShowZonesPreview && lParam == 0) {
        lParam = 2500;  // 默认展示 2.5 秒分区预览蒙层
    }

    std::lock_guard lock(m_mutex);
    if (!isProcessAliveLocked()) {
        if (m_shouldRun.load(std::memory_order_acquire) && !m_circuitBreakerTripped) {
            launchManagedProcessLocked(TraceId::generate());
        }
        if (!isProcessAliveLocked()) {
            return {{"success", false}, {"error", "window kit is not running"}};
        }
    }

    if (code == kWindowKitActionOpenLayoutEditor) {
        nlohmann::json payload = {
            {"uiLang", resolveEffectiveUiLangShort()}
        };
        if (arg.is_object()) {
            if (arg.contains("monitorId") && arg["monitorId"].is_string()) {
                payload["monitorId"] = arg["monitorId"].get<std::string>();
            }
            if (arg.contains("desktopId") && arg["desktopId"].is_string()) {
                payload["desktopId"] = arg["desktopId"].get<std::string>();
            }
            if (arg.contains("layoutId") && arg["layoutId"].is_string()) {
                payload["layoutId"] = arg["layoutId"].get<std::string>();
            }
            if (arg.contains("restoreSettingsHwnd") && arg["restoreSettingsHwnd"].is_number_integer()) {
                const auto rh = arg["restoreSettingsHwnd"].get<int64_t>();
                payload["restoreSettingsHwnd"] = rh;
                lParam = static_cast<LPARAM>(rh);
            }
        }
        WinUtils::atomicWriteFileWithFlush(resolveActionPayloadPath().wstring(), payload.dump());
    }

    if (code == kWindowKitActionLaunchWorkspace) {
        if (workspaceId.empty() && m_settings.contains("workspaces") &&
            m_settings["workspaces"].contains("items") && m_settings["workspaces"]["items"].is_array()) {
            const auto& items = m_settings["workspaces"]["items"];
            const size_t idx = (lParam >= 0) ? static_cast<size_t>(lParam) : 0;
            if (idx < items.size() && items[idx].is_object()) {
                workspaceId = items[idx].value("id", std::string{});
            }
        }
        if (workspaceId.empty()) {
            return {{"success", false}, {"error", "workspaceId is required"}};
        }
        const nlohmann::json payload = {{"workspaceId", workspaceId}};
        WinUtils::atomicWriteFileWithFlush(resolveActionPayloadPath().wstring(), payload.dump());
    }

    postControlMessageLocked(kWindowKitMsgExecAction, code, lParam);
    return {{"success", true}, {"action", actionName}};
}

void WindowKitController::syncMutedFlags(bool fullscreenMuted, bool remoteMuted) {
    const uint32_t newFlags = (fullscreenMuted ? 0x01u : 0u) | (remoteMuted ? 0x02u : 0u);
    const uint32_t prev = m_mutedFlags.exchange(newFlags, std::memory_order_acq_rel);
    if (prev == newFlags) return;
    if (!m_shouldRun.load(std::memory_order_acquire)) return;

    std::lock_guard lock(m_mutex);
    if (isProcessAliveLocked()) {
        postControlMessageLocked(kWindowKitMsgSetMutedFlags, static_cast<WPARAM>(newFlags), 0);
    }
}

void WindowKitController::notifyHostEnvironmentChanged() {
    if (!m_shouldRun.load(std::memory_order_acquire)) return;
    std::lock_guard lock(m_mutex);
    if (isProcessAliveLocked()) {
        const std::string traceId = TraceId::generate();
        if (writeRuntimeConfigAtomicLocked(traceId)) {
            postControlMessageLocked(kWindowKitMsgReloadConfig, 0, 0);
        }
    }
}

nlohmann::json WindowKitController::runBoundedOneShotCli(
    const std::wstring& args,
    const std::filesystem::path& outputFile,
    DWORD timeoutMs) const {
    if (!isBinaryInstalled()) {
        return {{"success", false}, {"error", "Tools3000_WindowKit.exe not found"}};
    }

    std::error_code ec;
    std::filesystem::create_directories(outputFile.parent_path(), ec);
    std::filesystem::remove(outputFile, ec);

    const auto exePath = resolveBinaryPath();
    std::wstring cmdLine = L"\"";
    cmdLine += exePath.wstring();
    cmdLine += L"\" ";
    cmdLine += args;

    STARTUPINFOW si{};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESHOWWINDOW;
    si.wShowWindow = SW_HIDE;

    PROCESS_INFORMATION pi{};
    const BOOL ok = CreateProcessW(
        exePath.c_str(),
        cmdLine.data(),
        nullptr,
        nullptr,
        FALSE,
        CREATE_NO_WINDOW | CREATE_UNICODE_ENVIRONMENT,
        nullptr,
        exePath.parent_path().c_str(),
        &si,
        &pi);
    if (!ok) {
        return {{"success", false}, {"error", "failed to spawn window kit helper"}};
    }

    WinUtils::assignProcessToCurrentJob(pi.hProcess);
    CloseHandle(pi.hThread);

    const DWORD waitRes = WaitForSingleObject(pi.hProcess, timeoutMs);
    DWORD exitCode = 1;
    if (waitRes != WAIT_OBJECT_0) {
        LOG_WARN("[WindowKit] One-shot CLI timed out after {}ms; terminating helper", timeoutMs);
        TerminateProcess(pi.hProcess, 1);
        WaitForSingleObject(pi.hProcess, 500);
        CloseHandle(pi.hProcess);
        std::filesystem::remove(outputFile, ec);
        return {{"success", false}, {"error", "helper timed out"}};
    }
    GetExitCodeProcess(pi.hProcess, &exitCode);
    CloseHandle(pi.hProcess);

    const std::string body = readFileUtf8(outputFile);
    std::filesystem::remove(outputFile, ec);
    if (exitCode != 0 || body.empty()) {
        return {{"success", false}, {"error", "helper returned no data"}};
    }

    try {
        return {{"success", true}, {"data", nlohmann::json::parse(body)}};
    } catch (const std::exception& e) {
        return {{"success", false}, {"error", std::string("invalid helper json: ") + e.what()}};
    }
}

nlohmann::json WindowKitController::getMonitorsJson() {
    const auto outFile = WinUtils::getConfigDirectory() /
        (L"wk_monitors_" + std::to_wstring(GetTickCount64()) + L".json");
    const auto res = runBoundedOneShotCli(L"--dump-monitors \"" + outFile.wstring() + L"\"", outFile, 4000);
    if (!res.value("success", false)) {
        return res;
    }
    nlohmann::json rawMonitors = nlohmann::json::array();
    if (res["data"].is_object() && res["data"].contains("monitors") && res["data"]["monitors"].is_array()) {
        rawMonitors = res["data"]["monitors"];
    } else if (res["data"].is_array()) {
        rawMonitors = res["data"];
    }

    nlohmann::json normalized = nlohmann::json::array();
    int idx = 1;
    for (const auto& m : rawMonitors) {
        if (!m.is_object()) continue;
        int w = m.value("width", 0);
        int h = m.value("height", 0);
        if ((w <= 0 || h <= 0) && m.contains("rect") && m["rect"].is_object()) {
            const auto& r = m["rect"];
            w = r.value("right", 0) - r.value("left", 0);
            h = r.value("bottom", 0) - r.value("top", 0);
        }
        const std::string id = m.value("id", "MONITOR-" + std::to_string(idx));
        std::string name = m.value("name", m.value("deviceName", "Display " + std::to_string(idx)));
        if (name.empty() || name.rfind("\\\\.\\", 0) == 0) {
            name = "Display " + std::to_string(idx);
        }
        normalized.push_back({
            {"id", id},
            {"name", name},
            {"width", std::max(0, w)},
            {"height", std::max(0, h)},
            {"dpi", m.value("dpi", 96)},
            {"isPrimary", m.value("isPrimary", idx == 1)}
        });
        ++idx;
    }
    const auto vd = getVirtualDesktopsJson();
    return {
        {"success", true},
        {"monitors", normalized},
        {"virtualDesktops", vd.value("desktops", nlohmann::json::array())},
        {"currentDesktopId", vd.value("currentDesktopId", std::string{})}
    };
}

nlohmann::json WindowKitController::getVirtualDesktopsJson() const {
    nlohmann::json desktops = nlohmann::json::array();
    std::string currentDesktopId;

    HKEY hKey = nullptr;
    if (RegOpenKeyExW(
            HKEY_CURRENT_USER,
            L"Software\\Microsoft\\Windows\\CurrentVersion\\Explorer\\VirtualDesktops",
            0,
            KEY_READ,
            &hKey) == ERROR_SUCCESS) {
        DWORD curType = 0;
        uint8_t curBuf[16]{};
        DWORD curSize = sizeof(curBuf);
        if (RegQueryValueExW(hKey, L"CurrentVirtualDesktop", nullptr, &curType, curBuf, &curSize) == ERROR_SUCCESS &&
            curType == REG_BINARY && curSize == 16) {
            currentDesktopId = formatGuidBytesLower(curBuf);
        }

        DWORD listType = 0;
        DWORD listSize = 0;
        if (RegQueryValueExW(hKey, L"VirtualDesktopIDs", nullptr, &listType, nullptr, &listSize) == ERROR_SUCCESS &&
            listType == REG_BINARY && listSize >= 16 && listSize <= 16 * 64) {
            std::vector<uint8_t> raw(listSize);
            if (RegQueryValueExW(hKey, L"VirtualDesktopIDs", nullptr, &listType, raw.data(), &listSize) == ERROR_SUCCESS) {
                const size_t count = listSize / 16;
                for (size_t i = 0; i < count; ++i) {
                    const std::string guid = formatGuidBytesLower(raw.data() + i * 16);
                    if (guid.empty()) continue;
                    std::string customName = queryVirtualDesktopCustomName(guid);
                    desktops.push_back({
                        {"id", guid},
                        {"name", customName},
                        {"index", static_cast<int>(i + 1)},
                        {"isCurrent", !currentDesktopId.empty() && guid == currentDesktopId}
                    });
                }
            }
        }
        RegCloseKey(hKey);
    }

    if (desktops.empty() && !currentDesktopId.empty()) {
        desktops.push_back({
            {"id", currentDesktopId},
            {"name", queryVirtualDesktopCustomName(currentDesktopId)},
            {"index", 1},
            {"isCurrent", true}
        });
    }

    return {
        {"success", true},
        {"currentDesktopId", currentDesktopId},
        {"desktops", desktops}
    };
}

bool WindowKitController::handleLayoutEditorResult(bool saved) {
    const auto resultPath = resolveEditorResultPath();
    if (!saved) {
        std::error_code ec;
        std::filesystem::remove(resultPath, ec);
        return false;
    }

    const std::string raw = readFileUtf8(resultPath);
    std::error_code ec;
    std::filesystem::remove(resultPath, ec);
    if (raw.empty()) {
        LOG_WARN("[WindowKit] Layout editor result file was empty");
        return false;
    }

    nlohmann::json payload;
    try {
        payload = nlohmann::json::parse(raw);
    } catch (const std::exception& e) {
        LOG_ERROR("[WindowKit] Failed to parse layout editor result JSON: {}", e.what());
        return false;
    }
    if (!payload.is_object() || !payload.value("saved", false) ||
        !payload.contains("layout") || !payload["layout"].is_object()) {
        return false;
    }

    const nlohmann::json uiLayout = translateRustLayoutToUi(payload["layout"]);
    const std::string layoutId = uiLayout.value("id", std::string{});
    if (layoutId.empty()) {
        return false;
    }
    const std::string monitorId = payload.value("monitorId", std::string{});
    const std::string desktopId = payload.value("desktopId", std::string{});
    const bool perDesktop = payload.value("perVirtualDesktop", false);
    const int zoneGap = payload.value("zoneGap", -1);

    {
        std::lock_guard lock(m_mutex);
        if (!m_initialized) {
            loadFromConfigLocked();
            m_initialized = true;
        }
        auto& sz = m_settings["snapZones"];
        if (!sz.is_object()) {
            sz = buildDefaultSettingsJson()["snapZones"];
        }
        if (!sz.contains("layouts") || !sz["layouts"].is_array()) {
            sz["layouts"] = buildBuiltinZoneLayouts();
        }
        bool updatedExisting = false;
        for (auto& existing : sz["layouts"]) {
            if (existing.is_object() && existing.value("id", std::string{}) == layoutId) {
                existing = uiLayout;
                updatedExisting = true;
                break;
            }
        }
        if (!updatedExisting) {
            sz["layouts"].push_back(uiLayout);
        }
        if (zoneGap >= 0 && zoneGap <= 48) {
            sz["zoneGap"] = zoneGap;
        }
        if (!sz.contains("monitorBindings") || !sz["monitorBindings"].is_object()) {
            sz["monitorBindings"] = nlohmann::json::object();
        }
        if (!monitorId.empty()) {
            const std::string bindingKey =
                (perDesktop && !desktopId.empty()) ? (monitorId + "@" + desktopId) : monitorId;
            sz["monitorBindings"][bindingKey] = layoutId;
        }

        saveToConfigLocked();
        reconcileProcessStateLocked(false);
    }

    pushStateChangedEvents();
    LOG_INFO("[WindowKit] Persisted 1:1 fullscreen layout editor result id={} monitor={} desktop={}",
             layoutId, monitorId, desktopId);
    return true;
}

nlohmann::json WindowKitController::captureWorkspaceJson(const std::string& name) {
    const auto outFile = WinUtils::getConfigDirectory() /
        (L"wk_capture_" + std::to_wstring(GetTickCount64()) + L".json");
    const auto res = runBoundedOneShotCli(L"--capture-workspace \"" + outFile.wstring() + L"\"", outFile, 4500);
    if (!res.value("success", false)) {
        return res;
    }
    nlohmann::json apps = nlohmann::json::array();
    if (res["data"].is_object() && res["data"].contains("windows") && res["data"]["windows"].is_array()) {
        for (const auto& w : res["data"]["windows"]) {
            if (!w.is_object()) continue;
            nlohmann::json entry = {
                {"exePath", w.value("exePath", std::string{})},
                {"processName", w.value("processName", std::string{})},
                {"args", w.value("args", std::string{})},
                {"monitorId", w.value("monitorId", std::string{})},
                {"maximized", w.value("maximized", false)}
            };
            if (w.contains("rect") && w["rect"].is_object()) {
                const auto& r = w["rect"];
                const int left = r.value("left", r.value("x", 0));
                const int top = r.value("top", r.value("y", 0));
                const int width = r.contains("w") ? r.value("w", 0) : (r.value("right", left) - left);
                const int height = r.contains("h") ? r.value("h", 0) : (r.value("bottom", top) - top);
                entry["rect"] = {{"x", left}, {"y", top}, {"w", std::max(0, width)}, {"h", std::max(0, height)}};
            }
            apps.push_back(std::move(entry));
        }
    }

    const std::string wsId = "ws-" + std::to_string(GetTickCount64());
    const std::string wsName = name.empty() ? ("Workspace " + std::to_string(GetTickCount64() % 1000)) : name;
    return {
        {"success", true},
        {"workspace", {
            {"id", wsId},
            {"name", wsName},
            {"apps", apps}
        }}
    };
}

nlohmann::json WindowKitController::importPowerToysLayoutsJson(const std::string& sourcePath) {
    const auto outFile = WinUtils::getConfigDirectory() /
        (L"wk_pt_import_" + std::to_wstring(GetTickCount64()) + L".json");
    std::wstring args = L"--import-powertoys \"" + outFile.wstring() + L"\"";
    if (!sourcePath.empty()) {
        args += L" --source \"";
        args += WinUtils::utf8ToWstring(sourcePath);
        args += L"\"";
    }
    const auto res = runBoundedOneShotCli(args, outFile, 4500);
    if (!res.value("success", false)) {
        return res;
    }

    nlohmann::json uiLayouts = nlohmann::json::array();
    if (res["data"].is_object() && res["data"].contains("layouts") && res["data"]["layouts"].is_array()) {
        for (const auto& l : res["data"]["layouts"]) {
            uiLayouts.push_back(translateRustLayoutToUi(l));
        }
    }
    return {
        {"success", true},
        {"importedCount", uiLayouts.size()},
        {"layouts", uiLayouts}
    };
}

nlohmann::json WindowKitController::getRuntimeStatsJson() const {
    std::lock_guard lock(m_mutex);
    return buildRuntimeStatsLocked();
}

nlohmann::json WindowKitController::listSearchableWorkspacesJson() const {
    std::lock_guard lock(m_mutex);
    if (!m_masterEnabled ||
        !m_settings.contains("features") ||
        !m_settings["features"].value("workspaces", false)) {
        return nlohmann::json::array();
    }
    if (!m_settings.contains("workspaces") ||
        !m_settings["workspaces"].is_object() ||
        !m_settings["workspaces"].contains("items") ||
        !m_settings["workspaces"]["items"].is_array()) {
        return nlohmann::json::array();
    }
    return m_settings["workspaces"]["items"];
}

void WindowKitController::pushStateChangedEvents() const {
    auto& mb = MessageBridge::instance();
    const auto response = getSettingsJson();
    mb.pushEvent("windowManager:stateChanged", response);
    mb.pushEvent("plugins:changed", {
        {"id", "window_manager"},
        {"enabled", response.value("enabled", false)}
    });
}

}  // namespace tools3000::core
