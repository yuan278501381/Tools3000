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
#include <shlobj.h>
#include <shobjidl.h>

#include <algorithm>
#include <cmath>
#include <fstream>
#include <sstream>
#include <unordered_set>

namespace {

constexpr DWORD kGracefulShutdownTimeoutMs = 1500;
constexpr int kCrashWindowSeconds = 30;
constexpr size_t kMaxCrashesInWindow = 2;

const char* const kWindowKitHotkeyNames[] = {
    "WindowKit: Toggle Always on Top",
    "WindowKit: Move Next Zone",
    "WindowKit: Move Prev Zone",
    "WindowKit: Restore From Zone",
    "WindowKit: Move Next Monitor",
    "WindowKit: Rescue Offscreen",
    "WindowKit: Minimize To Tray",
    "WindowKit: Restore All Tray",
    "WindowKit: Opacity Up",
    "WindowKit: Opacity Down",
    "WindowKit: Opacity Reset",
    "WindowKit: Open Layout Editor",
    "WindowKit: Open Quick Switch",
};

std::string toLowerAsciiCopy(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    return s;
}

std::string sanitizeShortcutFileName(const std::string& rawName) {
    std::string cleaned;
    cleaned.reserve(rawName.size());
    for (unsigned char c : rawName) {
        if (c < 0x20 || c == '<' || c == '>' || c == ':' || c == '"' ||
            c == '/' || c == '\\' || c == '|' || c == '?' || c == '*') {
            cleaned.push_back('_');
        } else {
            cleaned.push_back(static_cast<char>(c));
        }
    }
    while (!cleaned.empty() && (cleaned.front() == ' ' || cleaned.front() == '.')) {
        cleaned.erase(cleaned.begin());
    }
    while (!cleaned.empty() && (cleaned.back() == ' ' || cleaned.back() == '.')) {
        cleaned.pop_back();
    }
    if (cleaned.empty()) {
        cleaned = "Tools3000 Workspace";
    }
    if (cleaned.size() > 80) {
        cleaned.resize(80);
    }
    return cleaned;
}

std::string mapLegacyBuiltinLayoutId(const std::string& id) {
    if (id == "builtin-columns-2" || id == "preset_columns_2" || id == "preset_columns_3") {
        return "template_columns";
    }
    if (id == "builtin-priority-3" || id == "builtin-focus-split" ||
        id == "preset_ultrawide_121" || id == "preset_priority_grid") {
        return "template_priority_grid";
    }
    if (id == "builtin-grid-2x2" || id == "preset_quad") {
        return "template_grid";
    }
    if (id == "preset_vertical_3") {
        return "template_rows";
    }
    return id;
}

nlohmann::json normalizeRectToXywh(const nlohmann::json& r) {
    if (!r.is_object()) {
        return {{"x", 0}, {"y", 0}, {"w", 0}, {"h", 0}};
    }
    const int left = r.value("left", r.value("x", 0));
    const int top = r.value("top", r.value("y", 0));
    const int width = r.contains("w")
        ? r.value("w", 0)
        : (r.contains("width") ? r.value("width", 0) : (r.value("right", left) - left));
    const int height = r.contains("h")
        ? r.value("h", 0)
        : (r.contains("height") ? r.value("height", 0) : (r.value("bottom", top) - top));
    return {
        {"x", left},
        {"y", top},
        {"w", std::max(0, width)},
        {"h", std::max(0, height)}
    };
}

nlohmann::json normalizeWorkspaceAppForUi(const nlohmann::json& w, size_t seq) {
    nlohmann::json entry = {
        {"id", w.value("id", "app_" + std::to_string(seq + 1))},
        {"appName", w.value("appName", w.value("processName", std::string{}))},
        {"exePath", w.value("exePath", std::string{})},
        {"processName", w.value("processName", std::string{})},
        {"args", w.value("args", std::string{})},
        {"titleHint", w.value("titleHint", w.value("title", std::string{}))},
        {"aumid", w.value("aumid", std::string{})},
        {"iconDataUrl", w.value("iconDataUrl", std::string{})},
        {"monitorId", w.value("monitorId", std::string{})},
        {"monitorFriendly", w.value("monitorFriendly", std::string{})},
        {"monitorOrientation", w.value("monitorOrientation", std::string("landscape"))},
        {"monitorIndex", w.value("monitorIndex", 0)},
        {"maximized", w.value("maximized", false)},
        {"minimized", w.value("minimized", false)},
        {"runAsAdmin", w.value("runAsAdmin", false)},
        {"alwaysOnTop", w.value("alwaysOnTop", false)},
        {"opacityPercent", w.value("opacityPercent", 100)}
    };
    if (w.contains("rect") && w["rect"].is_object()) {
        entry["rect"] = normalizeRectToXywh(w["rect"]);
    }
    if (w.contains("relRect") && w["relRect"].is_object()) {
        entry["relRect"] = w["relRect"];
    }
    if (w.contains("zoneIndex") && w["zoneIndex"].is_number_integer()) {
        entry["zoneIndex"] = w["zoneIndex"];
    }
    if (w.contains("zoneIndices") && w["zoneIndices"].is_array()) {
        entry["zoneIndices"] = w["zoneIndices"];
    }
    return entry;
}

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
            {"id", "template_none"},
            {"name", "No layout"},
            {"type", "none"},
            {"builtin", true},
            {"templateKind", "none"},
            {"zoneCount", 0},
            {"showSpacing", false},
            {"gap", 0},
            {"highlightDistance", 20},
            {"defaultHorizontal", false},
            {"defaultVertical", false}
        },
        {
            {"id", "template_focus"},
            {"name", "Focus"},
            {"type", "focus"},
            {"builtin", true},
            {"templateKind", "focus"},
            {"zoneCount", 3},
            {"showSpacing", true},
            {"gap", 16},
            {"highlightDistance", 20},
            {"defaultHorizontal", false},
            {"defaultVertical", false}
        },
        {
            {"id", "template_columns"},
            {"name", "Columns"},
            {"type", "grid"},
            {"builtin", true},
            {"templateKind", "columns"},
            {"zoneCount", 3},
            {"showSpacing", true},
            {"gap", 16},
            {"highlightDistance", 20},
            {"defaultHorizontal", false},
            {"defaultVertical", false},
            {"rows", 1},
            {"columns", 3},
            {"rowPercents", {100}},
            {"colPercents", {33.3, 33.4, 33.3}},
            {"cellMap", {{0, 1, 2}}}
        },
        {
            {"id", "template_rows"},
            {"name", "Rows"},
            {"type", "grid"},
            {"builtin", true},
            {"templateKind", "rows"},
            {"zoneCount", 3},
            {"showSpacing", true},
            {"gap", 16},
            {"highlightDistance", 20},
            {"defaultHorizontal", false},
            {"defaultVertical", true},
            {"rows", 3},
            {"columns", 1},
            {"rowPercents", {33.3, 33.4, 33.3}},
            {"colPercents", {100}},
            {"cellMap", {{0}, {1}, {2}}}
        },
        {
            {"id", "template_grid"},
            {"name", "Grid"},
            {"type", "grid"},
            {"builtin", true},
            {"templateKind", "grid"},
            {"zoneCount", 3},
            {"showSpacing", true},
            {"gap", 16},
            {"highlightDistance", 20},
            {"defaultHorizontal", false},
            {"defaultVertical", false},
            {"rows", 1},
            {"columns", 3},
            {"rowPercents", {100}},
            {"colPercents", {33.3, 33.4, 33.3}},
            {"cellMap", {{0, 1, 2}}}
        },
        {
            {"id", "template_priority_grid"},
            {"name", "Priority Grid"},
            {"type", "grid"},
            {"builtin", true},
            {"templateKind", "priority_grid"},
            {"zoneCount", 3},
            {"showSpacing", true},
            {"gap", 16},
            {"highlightDistance", 20},
            {"defaultHorizontal", true},
            {"defaultVertical", false},
            {"rows", 1},
            {"columns", 3},
            {"rowPercents", {100}},
            {"colPercents", {25, 50, 25}},
            {"cellMap", {{0, 1, 2}}}
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

/// 将前端 SnapZoneLayout 转换为 Rust config.rs Schema v3 精确识别的布局 JSON（无缝兼容 None/Focus/Grid/Canvas）
nlohmann::json translateLayoutToRust(const nlohmann::json& uiLayout) {
    if (!uiLayout.is_object()) return nlohmann::json::object();
    nlohmann::json out = nlohmann::json::object();
    out["id"] = uiLayout.value("id", std::string("custom-layout"));
    out["name"] = uiLayout.value("name", out["id"].get<std::string>());
    const std::string kind = uiLayout.value("type", std::string("grid"));
    out["type"] = kind;
    out["builtIn"] = uiLayout.value("builtin", uiLayout.value("builtIn", false));
    if (uiLayout.contains("templateKind") && uiLayout["templateKind"].is_string() &&
        !uiLayout["templateKind"].get<std::string>().empty()) {
        out["templateKind"] = uiLayout["templateKind"];
    }
    if (uiLayout.contains("zoneCount") && uiLayout["zoneCount"].is_number_integer()) {
        out["zoneCount"] = uiLayout["zoneCount"];
    }
    if (uiLayout.contains("showSpacing") && uiLayout["showSpacing"].is_boolean()) {
        out["showSpacing"] = uiLayout["showSpacing"];
    }
    if (uiLayout.contains("gap") && uiLayout["gap"].is_number_integer()) {
        out["gap"] = uiLayout["gap"];
    }
    if (uiLayout.contains("highlightDistance") && uiLayout["highlightDistance"].is_number_integer()) {
        out["highlightDistance"] = uiLayout["highlightDistance"];
    }
    if (uiLayout.contains("hotkeyDigit") && uiLayout["hotkeyDigit"].is_number_integer()) {
        const int d = uiLayout["hotkeyDigit"].get<int>();
        if (d >= 0 && d <= 9) {
            out["hotkeyDigit"] = d;
        }
    }
    if (uiLayout.contains("defaultHorizontal") && uiLayout["defaultHorizontal"].is_boolean()) {
        out["defaultHorizontal"] = uiLayout["defaultHorizontal"];
    }
    if (uiLayout.contains("defaultVertical") && uiLayout["defaultVertical"].is_boolean()) {
        out["defaultVertical"] = uiLayout["defaultVertical"];
    }

    if (kind == "none" || kind == "focus") {
        return out;
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

/// 将 Rust 输出的 Schema v3 布局转换为前端精确识别的 SnapZoneLayout
nlohmann::json translateRustLayoutToUi(const nlohmann::json& rustLayout) {
    if (!rustLayout.is_object()) return nlohmann::json::object();
    const std::string kind = rustLayout.value("type", std::string("grid"));
    nlohmann::json out = {
        {"id", rustLayout.value("id", std::string("imported"))},
        {"name", rustLayout.value("name", std::string("Imported Layout"))},
        {"type", kind},
        {"builtin", rustLayout.value("builtIn", rustLayout.value("builtin", false))}
    };
    if (rustLayout.contains("templateKind") && rustLayout["templateKind"].is_string()) {
        out["templateKind"] = rustLayout["templateKind"];
    }
    if (rustLayout.contains("zoneCount") && rustLayout["zoneCount"].is_number_integer()) {
        out["zoneCount"] = rustLayout["zoneCount"];
    }
    if (rustLayout.contains("showSpacing") && rustLayout["showSpacing"].is_boolean()) {
        out["showSpacing"] = rustLayout["showSpacing"];
    }
    if (rustLayout.contains("gap") && rustLayout["gap"].is_number_integer()) {
        out["gap"] = rustLayout["gap"];
    }
    if (rustLayout.contains("highlightDistance") && rustLayout["highlightDistance"].is_number_integer()) {
        out["highlightDistance"] = rustLayout["highlightDistance"];
    }
    if (rustLayout.contains("hotkeyDigit") && rustLayout["hotkeyDigit"].is_number_integer()) {
        const int d = rustLayout["hotkeyDigit"].get<int>();
        if (d >= 0 && d <= 9) {
            out["hotkeyDigit"] = d;
        }
    }
    if (rustLayout.contains("defaultHorizontal") && rustLayout["defaultHorizontal"].is_boolean()) {
        out["defaultHorizontal"] = rustLayout["defaultHorizontal"];
    }
    if (rustLayout.contains("defaultVertical") && rustLayout["defaultVertical"].is_boolean()) {
        out["defaultVertical"] = rustLayout["defaultVertical"];
    }

    if (kind == "none" || kind == "focus") {
        return out;
    }

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
            {"edgeThresholdPx", 20},
            {"spanMultipleZones", true},
            {"showZoneNumbers", true},
            {"zoneGap", 16},
            {"overlayOpacity", 70},
            {"rememberLastZone", true},
            {"restoreOriginalSize", true},
            {"perVirtualDesktop", false},
            {"showOnAllMonitors", false},
            {"overrideWinArrows", false},
            {"moveByPosition", false},
            {"activeLayoutId", "template_priority_grid"},
            {"excludedApps", nlohmann::json::array()},
            {"layouts", buildBuiltinZoneLayouts()},
            {"monitorBindings", nlohmann::json::object()}
        }},
        {"workspaces", {
            {"minimizeUnrelated", false},
            {"unrelatedAction", "keep"},
            {"items", nlohmann::json::array()}
        }},
        {"quickSwitch", {
            {"replaceAltTab", true},
            {"sameAppEnabled", false},
            {"sameAppHotkey", "Alt+`"},
            {"currentMonitorOnly", false},
            {"currentDesktopOnly", true},
            {"showPreview", true},
            {"previewInDesktopPosition", false},
            {"showNumbers", true},
            {"showAppNames", false},
            {"largeIcons", true},
            {"visibleBorders", true},
            {"density", "comfortable"},
            {"enablePinyinSearch", true},
            {"rememberBounds", true},
            {"listWidthRatio", 0.34},
            {"customMonitorId", ""}
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
            {"restoreFromZone", "Win+Alt+Down"},
            {"moveNextMonitor", "Ctrl+Win+Right"},
            {"rescueOffscreen", "Ctrl+Win+G"},
            {"minimizeToTray", "Ctrl+Win+Down"},
            {"restoreAllTray", "Ctrl+Win+Shift+Down"},
            {"opacityUp", "Ctrl+Win+Up"},
            {"opacityDown", "Ctrl+Win+PageDown"},
            {"opacityReset", "Ctrl+Win+0"},
            {"openLayoutEditor", "Win+Shift+~"},
            {"openQuickSwitch", ""}
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
    if (actionName == "open_quick_switch" || actionName == "showQuickSwitch" || actionName == "openQuickSwitch") return kWindowKitActionOpenQuickSwitch;
    if (actionName == "capture_workspace" || actionName == "captureWorkspace") return kWindowKitActionCaptureWorkspace;
    if (actionName == "launch_workspace" || actionName == "launchWorkspace") return kWindowKitActionLaunchWorkspace;
    if (actionName == "import_powertoys_zones") return kWindowKitActionImportPowerToysZones;
    if (actionName == "query_monitors") return kWindowKitActionQueryMonitors;
    if (actionName == "query_runtime_stats") return kWindowKitActionQueryRuntimeStats;
    if (actionName == "show_zones_preview" || actionName == "showZoneOverlayPreview") return kWindowKitActionShowZonesPreview;
    if (actionName == "open_layout_editor" || actionName == "openLayoutEditor" || actionName == "open_layout_hub" || actionName == "openLayoutHub") return kWindowKitActionOpenLayoutEditor;
    if (actionName == "apply_layout_hotkey_digit" || actionName == "applyLayoutHotkeyDigit") return kWindowKitActionApplyLayoutHotkeyDigit;
    if (actionName == "start_workspace_capture" || actionName == "startWorkspaceCapture") return kWindowKitActionStartWorkspaceCapture;
    if (actionName == "preview_border_style" || actionName == "previewBorderStyle") return kWindowKitActionPreviewBorderStyle;
    if (actionName == "open_layout_editor_direct" || actionName == "openLayoutEditorDirect") return kWindowKitActionOpenLayoutEditorDirect;
    if (actionName == "apply_layout_by_id" || actionName == "applyLayoutById") return kWindowKitActionApplyLayoutById;
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

std::filesystem::path WindowKitController::resolveWorkspaceCaptureResultPath() const {
    return WinUtils::getConfigDirectory() / L"window_kit_workspace_capture_result.json";
}

std::filesystem::path WindowKitController::resolveQuickSwitchBoundsPath() const {
    return WinUtils::getConfigDirectory() / L"window_kit_quickswitch_bounds.json";
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
    auto& sz = m_settings["snapZones"];
    if (!sz.contains("layouts") || !sz["layouts"].is_array() || sz["layouts"].empty()) {
        sz["layouts"] = buildBuiltinZoneLayouts();
    } else {
        // 无感升级老版本 4 内置布局至 6 大参数化模板，100% 保留用户已有自定义布局与绑定
        bool hasNewTemplates = false;
        for (const auto& l : sz["layouts"]) {
            if (l.is_object() && l.value("id", std::string{}).rfind("template_", 0) == 0) {
                hasNewTemplates = true;
                break;
            }
        }
        if (!hasNewTemplates) {
            nlohmann::json upgraded = buildBuiltinZoneLayouts();
            for (const auto& l : sz["layouts"]) {
                if (!l.is_object()) continue;
                const bool isBuiltin = l.value("builtin", l.value("builtIn", false));
                if (!isBuiltin) {
                    upgraded.push_back(l);
                }
            }
            sz["layouts"] = std::move(upgraded);
            if (sz.contains("monitorBindings") && sz["monitorBindings"].is_object()) {
                for (auto& [_, val] : sz["monitorBindings"].items()) {
                    if (val.is_string()) {
                        val = mapLegacyBuiltinLayoutId(val.get<std::string>());
                    }
                }
            }
            if (sz.contains("activeLayoutId") && sz["activeLayoutId"].is_string()) {
                sz["activeLayoutId"] = mapLegacyBuiltinLayoutId(sz["activeLayoutId"].get<std::string>());
            }
        }
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
    const std::string theme = cfg.get<std::string>("/general/theme", "system");
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

    // 转换 SnapZones 布局列表为 Rust Schema v3 精确格式
    nlohmann::json rustLayouts = nlohmann::json::array();
    std::string firstLayoutId = "template_priority_grid";
    if (sz.contains("layouts") && sz["layouts"].is_array()) {
        for (const auto& l : sz["layouts"]) {
            auto translated = translateLayoutToRust(l);
            if (translated.is_object() && !translated.value("id", std::string{}).empty()) {
                if (rustLayouts.empty() && translated.value("type", std::string{}) != "none") {
                    firstLayoutId = translated["id"].get<std::string>();
                }
                rustLayouts.push_back(std::move(translated));
            }
        }
    }
    const std::string configuredActiveLayout = sz.value("activeLayoutId", std::string{});
    const std::string activeLayoutId = configuredActiveLayout.empty() ? firstLayoutId : configuredActiveLayout;

    std::string triggerMode = sz.value("triggerMode", std::string("shift"));
    if (triggerMode == "edge") triggerMode = "top_edge";

    const double overlayOpacityPct = sz.value("overlayOpacity", 70.0);
    const double highlightOpacity = std::clamp(overlayOpacityPct / 100.0 * 0.45, 0.08, 0.80);

    nlohmann::json rustQuickSwitch = {
        {"replaceAltTab", qs.value("replaceAltTab", true)},
        {"sameAppEnabled", qs.value("sameAppEnabled", false)},
        {"sameAppHotkey", qs.value("sameAppHotkey", std::string("Alt+`"))},
        {"showLivePreview", qs.value("showPreview", true)},
        {"previewInDesktopPosition", qs.value("previewInDesktopPosition", false)},
        {"showAllMonitors", !qs.value("currentMonitorOnly", false)},
        {"currentDesktopOnly", qs.value("currentDesktopOnly", true)},
        {"showProcessBadge", true},
        {"showAppNames", qs.value("showAppNames", false)},
        {"showNumbers", qs.value("showNumbers", true)},
        {"density", qs.value("density", std::string("comfortable"))},
        {"largeIcons", qs.value("largeIcons", true)},
        {"visibleBorders", qs.value("visibleBorders", true)},
        {"enablePinyinSearch", qs.value("enablePinyinSearch", true)},
        {"includeMinimized", true},
        {"rememberBounds", qs.value("rememberBounds", true)},
        {"listWidthRatio", qs.value("listWidthRatio", 0.34)},
        {"customMonitorId", qs.value("customMonitorId", std::string{})}
    };
    if (qs.contains("customWidthDip") && qs["customWidthDip"].is_number()) {
        rustQuickSwitch["customWidthDip"] = qs["customWidthDip"];
    }
    if (qs.contains("customHeightDip") && qs["customHeightDip"].is_number()) {
        rustQuickSwitch["customHeightDip"] = qs["customHeightDip"];
    }
    if (qs.contains("customRelRect") && qs["customRelRect"].is_object()) {
        rustQuickSwitch["customRelRect"] = qs["customRelRect"];
    }

    return {
        {"schemaVersion", 3},
        {"traceId", traceId},
        {"enabled", m_masterEnabled},
        {"theme", theme},
        {"accentColor", accentColor},
        {"uiLang", resolveEffectiveUiLangShort()},
        {"logDir", WinUtils::pathToUtf8(WinUtils::getLogDirectory())},
        {"logLevel", logLevel},
        {"excludedApps", globalExcluded},
        {"features", features},
        {"quickSwitch", std::move(rustQuickSwitch)},
        {"snapZones", {
            {"triggerMode", triggerMode},
            {"edgeThresholdPx", sz.value("edgeThresholdPx", 20)},
            {"zoneGap", sz.value("zoneGap", 16)},
            {"highlightOpacity", highlightOpacity},
            {"showZoneNumbers", sz.value("showZoneNumbers", true)},
            {"allowMultiZoneSpan", sz.value("spanMultipleZones", true)},
            {"rememberLastZone", sz.value("rememberLastZone", true)},
            {"restoreOriginalSize", sz.value("restoreOriginalSize", true)},
            {"perVirtualDesktop", sz.value("perVirtualDesktop", false)},
            {"showOnAllMonitors", sz.value("showOnAllMonitors", false)},
            {"overrideWinArrows", sz.value("overrideWinArrows", false)},
            {"moveByPosition", sz.value("moveByPosition", false)},
            {"excludedApps", snapExcluded},
            {"activeLayoutId", activeLayoutId},
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
            {"unrelatedAction", ws.value("unrelatedAction", std::string(ws.value("minimizeUnrelated", false) ? "minimize" : "keep"))},
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

HWND WindowKitController::waitForManagedControlWindowLocked(DWORD maxWaitMs) const noexcept {
    HWND ctrl = findManagedControlWindowLocked();
    if (ctrl && IsWindow(ctrl)) return ctrl;
    const ULONGLONG deadline = GetTickCount64() + maxWaitMs;
    while (GetTickCount64() < deadline && isProcessAliveLocked()) {
        Sleep(10);
        ctrl = findManagedControlWindowLocked();
        if (ctrl && IsWindow(ctrl)) return ctrl;
    }
    return nullptr;
}

void WindowKitController::postControlMessageLocked(UINT msg, WPARAM wp, LPARAM lp, bool waitIfJustStarted) const noexcept {
    if (!isProcessAliveLocked()) return;
    HWND ctrl = waitIfJustStarted ? waitForManagedControlWindowLocked(450) : findManagedControlWindowLocked();
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
    for (int d = 0; d <= 9; ++d) {
        hm.unregisterHotkey("WindowKit: Layout Hotkey " + std::to_string(d));
    }
    for (const auto& [regName, _] : m_registeredHotkeys) {
        hm.unregisterHotkey(regName);
    }
    m_registeredHotkeys.clear();
}

void WindowKitController::syncHotkeysLocked() {
    struct DesiredHotkey {
        std::string binding;
        std::string action;
        nlohmann::json arg;
    };
    std::map<std::string, DesiredHotkey> desired;
    if (hasAnyActiveFeature(m_masterEnabled, m_settings)) {
        const auto& features = m_settings["features"];
        const auto hotkeys = m_settings.value("hotkeys", nlohmann::json::object());

        auto collectIfEnabled = [&](const char* featureKey, const char* hkKey, const char* regName, const char* actionName) {
            if (!features.value(featureKey, false)) return;
            const std::string binding = hotkeys.value(hkKey, std::string{});
            if (binding.empty()) return;
            desired.emplace(regName, DesiredHotkey{binding, std::string(actionName), nullptr});
        };

        collectIfEnabled("alwaysOnTop", "toggleAlwaysOnTop", kWindowKitHotkeyNames[0], "toggle_always_on_top");
        collectIfEnabled("snapZones", "moveNextZone", kWindowKitHotkeyNames[1], "move_next_zone");
        collectIfEnabled("snapZones", "movePrevZone", kWindowKitHotkeyNames[2], "move_prev_zone");
        collectIfEnabled("snapZones", "restoreFromZone", kWindowKitHotkeyNames[3], "restore_from_zone");
        collectIfEnabled("multiMonitor", "moveNextMonitor", kWindowKitHotkeyNames[4], "move_next_monitor");
        collectIfEnabled("multiMonitor", "rescueOffscreen", kWindowKitHotkeyNames[5], "rescue_offscreen");
        collectIfEnabled("multiMonitor", "minimizeToTray", kWindowKitHotkeyNames[6], "minimize_to_tray");
        collectIfEnabled("multiMonitor", "restoreAllTray", kWindowKitHotkeyNames[7], "restore_all_tray");
        collectIfEnabled("opacity", "opacityUp", kWindowKitHotkeyNames[8], "opacity_up");
        collectIfEnabled("opacity", "opacityDown", kWindowKitHotkeyNames[9], "opacity_down");
        collectIfEnabled("opacity", "opacityReset", kWindowKitHotkeyNames[10], "opacity_reset");
        collectIfEnabled("snapZones", "openLayoutEditor", kWindowKitHotkeyNames[11], "open_layout_editor");
        collectIfEnabled("quickSwitch", "openQuickSwitch", kWindowKitHotkeyNames[12], "open_quick_switch");

        // 为绑定了 0..9 快捷数字的自定义布局动态注册 Win+Ctrl+Alt+{digit}
        if (features.value("snapZones", false) &&
            m_settings.contains("snapZones") && m_settings["snapZones"].is_object() &&
            m_settings["snapZones"].contains("layouts") && m_settings["snapZones"]["layouts"].is_array()) {
            for (const auto& lay : m_settings["snapZones"]["layouts"]) {
                if (!lay.is_object() || !lay.contains("hotkeyDigit") || !lay["hotkeyDigit"].is_number_integer()) {
                    continue;
                }
                const int digit = lay["hotkeyDigit"].get<int>();
                if (digit < 0 || digit > 9) continue;
                const std::string regName = "WindowKit: Layout Hotkey " + std::to_string(digit);
                const std::string binding = "Ctrl+Alt+Win+" + std::to_string(digit);
                desired[regName] = DesiredHotkey{binding, "apply_layout_hotkey_digit", nlohmann::json(digit)};
            }
        }
    }

    // 快照 Diff 去重：若目标热键集合与当前已注册集合 100% 一致，直接跳过，避免调滑块/切开关狂刷热键日志
    std::map<std::string, std::string> nextSnapshot;
    for (const auto& [regName, info] : desired) {
        nextSnapshot.emplace(regName, info.binding);
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
    for (const auto& [regName, info] : desired) {
        const auto oldIt = m_registeredHotkeys.find(regName);
        if (oldIt != m_registeredHotkeys.end() && oldIt->second == info.binding) {
            actualSnapshot.emplace(regName, info.binding);
            continue;
        }
        if (auto parsed = HotkeyDef::fromString(info.binding)) {
            const std::string act = info.action;
            const nlohmann::json argCopy = info.arg;
            hm.registerHotkey(regName, *parsed, [act, argCopy]() {
                WindowKitController::instance().execAction(act, argCopy);
            });
            actualSnapshot.emplace(regName, info.binding);
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

    bool shouldPushEvents = false;
    bool justLaunched = false;
    {
        std::lock_guard lock(m_mutex);
        if (!isProcessAliveLocked()) {
            if (m_shouldRun.load(std::memory_order_acquire) && !m_circuitBreakerTripped) {
                justLaunched = launchManagedProcessLocked(TraceId::generate());
            }
            if (!isProcessAliveLocked()) {
                return {{"success", false}, {"error", "window kit is not running"}};
            }
        }

        if (code == kWindowKitActionOpenLayoutEditor || code == kWindowKitActionOpenLayoutEditorDirect) {
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
                if (arg.contains("directEditor") && arg["directEditor"].is_boolean()) {
                    payload["directEditor"] = arg["directEditor"].get<bool>();
                }
                if (arg.contains("restoreSettingsHwnd") && arg["restoreSettingsHwnd"].is_number_integer()) {
                    const auto rh = arg["restoreSettingsHwnd"].get<int64_t>();
                    payload["restoreSettingsHwnd"] = rh;
                    lParam = static_cast<LPARAM>(rh);
                }
            }
            WinUtils::atomicWriteFileWithFlush(resolveActionPayloadPath().wstring(), payload.dump());
        }

        if (code == kWindowKitActionStartWorkspaceCapture) {
            nlohmann::json payload = {
                {"uiLang", resolveEffectiveUiLangShort()}
            };
            if (arg.is_object()) {
                if (arg.contains("workspaceId") && arg["workspaceId"].is_string()) {
                    payload["workspaceId"] = arg["workspaceId"].get<std::string>();
                }
                if (arg.contains("restoreSettingsHwnd") && arg["restoreSettingsHwnd"].is_number_integer()) {
                    const auto rh = arg["restoreSettingsHwnd"].get<int64_t>();
                    payload["restoreSettingsHwnd"] = rh;
                    lParam = static_cast<LPARAM>(rh);
                }
            }
            WinUtils::atomicWriteFileWithFlush(resolveActionPayloadPath().wstring(), payload.dump());
        }

        if (code == kWindowKitActionPreviewBorderStyle) {
            nlohmann::json payload = nlohmann::json::object();
            if (arg.is_object()) {
                const std::string kind = arg.value("kind", std::string("focus"));
                lParam = (kind == "alwaysOnTop" || kind == "pin") ? 1 : 0;
                if (arg.contains("borderColor") && arg["borderColor"].is_string()) {
                    payload["borderColor"] = arg["borderColor"].get<std::string>();
                } else if (arg.contains("color") && arg["color"].is_string()) {
                    payload["borderColor"] = arg["color"].get<std::string>();
                }
                if (arg.contains("borderWidth") && arg["borderWidth"].is_number_integer()) {
                    payload["borderWidth"] = arg["borderWidth"].get<int>();
                } else if (arg.contains("thickness") && arg["thickness"].is_number_integer()) {
                    payload["borderWidth"] = arg["thickness"].get<int>();
                } else if (arg.contains("borderThickness") && arg["borderThickness"].is_number_integer()) {
                    payload["borderWidth"] = arg["borderThickness"].get<int>();
                }
                if (arg.contains("cornerRadius") && arg["cornerRadius"].is_number_integer()) {
                    payload["cornerRadius"] = arg["cornerRadius"].get<int>();
                }
                if (arg.contains("opacity") && arg["opacity"].is_number()) {
                    payload["opacity"] = arg["opacity"].get<double>();
                }
            }
            WinUtils::atomicWriteFileWithFlush(resolveActionPayloadPath().wstring(), payload.dump());
        }

        if (code == kWindowKitActionApplyLayoutHotkeyDigit) {
            if (arg.is_object() && arg.contains("digit") && arg["digit"].is_number_integer()) {
                lParam = static_cast<LPARAM>(arg["digit"].get<int64_t>());
            }
        }

        if (code == kWindowKitActionApplyLayoutById) {
            std::string layoutId;
            if (arg.is_string()) {
                layoutId = arg.get<std::string>();
            } else if (arg.is_object() && arg.contains("layoutId") && arg["layoutId"].is_string()) {
                layoutId = arg["layoutId"].get<std::string>();
            }
            const nlohmann::json payload = {
                {"layoutId", layoutId},
                {"uiLang", resolveEffectiveUiLangShort()}
            };
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
            // 更新该工作区的 lastLaunchedAt（Unix 秒），驱动列表页上次启动时间与排序
            if (m_settings.contains("workspaces") &&
                m_settings["workspaces"].contains("items") && m_settings["workspaces"]["items"].is_array()) {
                const auto nowSec = static_cast<uint64_t>(
                    std::chrono::duration_cast<std::chrono::seconds>(
                        std::chrono::system_clock::now().time_since_epoch()).count());
                for (auto& item : m_settings["workspaces"]["items"]) {
                    if (item.is_object() && item.value("id", std::string{}) == workspaceId) {
                        item["lastLaunchedAt"] = nowSec;
                        saveToConfigLocked();
                        shouldPushEvents = true;
                        break;
                    }
                }
            }
            const nlohmann::json payload = {{"workspaceId", workspaceId}};
            WinUtils::atomicWriteFileWithFlush(resolveActionPayloadPath().wstring(), payload.dump());
        }

        postControlMessageLocked(kWindowKitMsgExecAction, code, lParam, justLaunched);
    }

    if (shouldPushEvents) {
        pushStateChangedEvents();
    }
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

    auto readGuidValue = [](HKEY root, const wchar_t* subKey, const wchar_t* valueName) -> std::string {
        HKEY hk = nullptr;
        if (RegOpenKeyExW(root, subKey, 0, KEY_READ, &hk) != ERROR_SUCCESS) {
            return {};
        }
        DWORD type = 0;
        uint8_t buf[16]{};
        DWORD cb = sizeof(buf);
        const LONG rc = RegQueryValueExW(hk, valueName, nullptr, &type, buf, &cb);
        RegCloseKey(hk);
        if (rc == ERROR_SUCCESS && type == REG_BINARY && cb == 16) {
            return formatGuidBytesLower(buf);
        }
        return {};
    };

    // 1. 优先读 Win11 / Server 2025 根目录 CurrentVirtualDesktop
    currentDesktopId = readGuidValue(
        HKEY_CURRENT_USER,
        L"Software\\Microsoft\\Windows\\CurrentVersion\\Explorer\\VirtualDesktops",
        L"CurrentVirtualDesktop");

    // 2. 兼容 Win10 (1809+) / Server 2019 / Server 2022 的 SessionInfo\{SessionId}\VirtualDesktops
    if (currentDesktopId.empty()) {
        DWORD sessionId = 0;
        if (ProcessIdToSessionId(GetCurrentProcessId(), &sessionId)) {
            const std::wstring sessionKey =
                L"Software\\Microsoft\\Windows\\CurrentVersion\\Explorer\\SessionInfo\\" +
                std::to_wstring(sessionId) + L"\\VirtualDesktops";
            currentDesktopId = readGuidValue(HKEY_CURRENT_USER, sessionKey.c_str(), L"CurrentVirtualDesktop");
        }
    }

    HKEY hKey = nullptr;
    if (RegOpenKeyExW(
            HKEY_CURRENT_USER,
            L"Software\\Microsoft\\Windows\\CurrentVersion\\Explorer\\VirtualDesktops",
            0,
            KEY_READ,
            &hKey) == ERROR_SUCCESS) {
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
    if (!payload.is_object() || !payload.value("saved", false)) {
        return false;
    }

    // 1. 双层共视布局管理器全量同步 (action == "hubSync")
    if (payload.value("action", std::string{}) == "hubSync") {
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
            if (payload.contains("layouts") && payload["layouts"].is_array() && !payload["layouts"].empty()) {
                nlohmann::json uiLayouts = nlohmann::json::array();
                for (const auto& l : payload["layouts"]) {
                    auto translated = translateRustLayoutToUi(l);
                    if (translated.is_object() && !translated.value("id", std::string{}).empty()) {
                        uiLayouts.push_back(std::move(translated));
                    }
                }
                if (!uiLayouts.empty()) {
                    sz["layouts"] = std::move(uiLayouts);
                }
            }
            if (payload.contains("monitorBindings")) {
                nlohmann::json bindingsMap = nlohmann::json::object();
                if (payload["monitorBindings"].is_array()) {
                    for (const auto& b : payload["monitorBindings"]) {
                        if (!b.is_object()) continue;
                        const std::string mid = b.value("monitorId", std::string{});
                        const std::string did = b.value("desktopId", std::string{});
                        const std::string lid = b.value("layoutId", std::string{});
                        if (mid.empty() || lid.empty()) continue;
                        const std::string key = did.empty() ? mid : (mid + "@" + did);
                        bindingsMap[key] = lid;
                    }
                } else if (payload["monitorBindings"].is_object()) {
                    bindingsMap = payload["monitorBindings"];
                }
                sz["monitorBindings"] = std::move(bindingsMap);
            }
            if (payload.contains("activeLayoutId") && payload["activeLayoutId"].is_string() &&
                !payload["activeLayoutId"].get<std::string>().empty()) {
                sz["activeLayoutId"] = payload["activeLayoutId"];
            }
            if (payload.contains("zoneGap") && payload["zoneGap"].is_number_integer()) {
                const int g = payload["zoneGap"].get<int>();
                if (g >= 0 && g <= 64) {
                    sz["zoneGap"] = g;
                }
            }

            saveToConfigLocked();
            syncHotkeysLocked();
            writeRuntimeConfigAtomicLocked(TraceId::generate());
        }

        pushStateChangedEvents();
        LOG_INFO("[WindowKit] Synced layout hub snapshot to host config");
        return true;
    }

    // 2. 传统单布局 1:1 全屏编辑器保存回写
    if (!payload.contains("layout") || !payload["layout"].is_object()) {
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
        if (zoneGap >= 0 && zoneGap <= 64) {
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

bool WindowKitController::handleWorkspaceCaptureResult(bool captured) {
    const auto resultPath = resolveWorkspaceCaptureResultPath();
    auto& mb = MessageBridge::instance();
    if (!captured) {
        std::error_code ec;
        std::filesystem::remove(resultPath, ec);
        const nlohmann::json cancelEvt = {{"captured", false}};
        mb.pushEvent("windowManager.workspaceCaptured", cancelEvt);
        mb.pushEvent("windowManager:workspaceCaptured", cancelEvt);
        return false;
    }

    const std::string raw = readFileUtf8(resultPath);
    std::error_code ec;
    std::filesystem::remove(resultPath, ec);
    if (raw.empty()) {
        LOG_WARN("[WindowKit] Workspace capture result file was empty");
        const nlohmann::json emptyEvt = {{"captured", false}};
        mb.pushEvent("windowManager.workspaceCaptured", emptyEvt);
        mb.pushEvent("windowManager:workspaceCaptured", emptyEvt);
        return false;
    }

    nlohmann::json parsed;
    try {
        parsed = nlohmann::json::parse(raw);
    } catch (const std::exception& e) {
        LOG_ERROR("[WindowKit] Failed to parse workspace capture JSON: {}", e.what());
        return false;
    }

    nlohmann::json apps = nlohmann::json::array();
    const auto& rawApps = (parsed.contains("apps") && parsed["apps"].is_array())
        ? parsed["apps"]
        : (parsed.contains("windows") && parsed["windows"].is_array()
               ? parsed["windows"]
               : nlohmann::json::array());
    size_t seq = 0;
    for (const auto& w : rawApps) {
        if (!w.is_object()) continue;
        apps.push_back(normalizeWorkspaceAppForUi(w, seq++));
    }

    nlohmann::json monitors = nlohmann::json::array();
    if (parsed.contains("monitors") && parsed["monitors"].is_array()) {
        int idx = 1;
        for (const auto& m : parsed["monitors"]) {
            if (!m.is_object()) continue;
            const auto rect = m.contains("rect") ? normalizeRectToXywh(m["rect"]) : nlohmann::json{{"x", 0}, {"y", 0}, {"w", m.value("width", 1920)}, {"h", m.value("height", 1080)}};
            const auto work = m.contains("workArea") ? normalizeRectToXywh(m["workArea"]) : rect;
            monitors.push_back({
                {"id", m.value("id", "MONITOR-" + std::to_string(idx))},
                {"name", m.value("name", m.value("friendlyName", "Screen " + std::to_string(idx)))},
                {"width", rect.value("w", 1920)},
                {"height", rect.value("h", 1080)},
                {"rect", rect},
                {"workArea", work},
                {"dpi", m.value("dpi", 96)},
                {"isPrimary", m.value("isPrimary", idx == 1)}
            });
            ++idx;
        }
    }

    const std::string targetWsId = parsed.value("targetWorkspaceId", std::string{});
    const std::string wsId = targetWsId.empty() ? ("ws-" + std::to_string(GetTickCount64())) : targetWsId;
    const nlohmann::json eventPayload = {
        {"captured", true},
        {"targetWorkspaceId", targetWsId},
        {"monitors", monitors},
        {"workspace", {
            {"id", wsId},
            {"name", ""},
            {"boundLayoutId", parsed.value("boundLayoutId", std::string{})},
            {"desktopId", parsed.value("desktopId", std::string{})},
            {"desktopName", parsed.value("desktopName", std::string{})},
            {"moveExistingWindows", true},
            {"unrelatedAction", "keep"},
            {"createDesktopShortcut", false},
            {"apps", apps}
        }}
    };
    mb.pushEvent("windowManager.workspaceCaptured", eventPayload);
    mb.pushEvent("windowManager:workspaceCaptured", eventPayload);
    LOG_INFO("[WindowKit] Delivered workspace capture snapshot (apps={}, targetWs='{}')",
             apps.size(), targetWsId);
    return true;
}

bool WindowKitController::handleQuickSwitchBoundsResult() {
    const auto boundsPath = resolveQuickSwitchBoundsPath();
    const std::string raw = readFileUtf8(boundsPath);
    std::error_code ec;
    std::filesystem::remove(boundsPath, ec);
    if (raw.empty()) {
        return false;
    }

    nlohmann::json parsed;
    try {
        parsed = nlohmann::json::parse(raw);
    } catch (const std::exception& e) {
        LOG_WARN("[WindowKit] Failed to parse quickswitch bounds JSON: {}", e.what());
        return false;
    }
    if (!parsed.is_object()) return false;

    {
        std::lock_guard lock(m_mutex);
        if (!m_initialized) {
            loadFromConfigLocked();
            m_initialized = true;
        }
        auto& qs = m_settings["quickSwitch"];
        if (!qs.is_object()) {
            qs = buildDefaultSettingsJson()["quickSwitch"];
        }
        if (parsed.contains("customWidthDip") && parsed["customWidthDip"].is_number()) {
            qs["customWidthDip"] = parsed["customWidthDip"];
        } else {
            qs.erase("customWidthDip");
        }
        if (parsed.contains("customHeightDip") && parsed["customHeightDip"].is_number()) {
            qs["customHeightDip"] = parsed["customHeightDip"];
        } else {
            qs.erase("customHeightDip");
        }
        if (parsed.contains("listWidthRatio") && parsed["listWidthRatio"].is_number()) {
            qs["listWidthRatio"] = std::clamp(parsed["listWidthRatio"].get<double>(), 0.22, 0.60);
        }
        if (parsed.contains("customMonitorId") && parsed["customMonitorId"].is_string()) {
            qs["customMonitorId"] = parsed["customMonitorId"];
        }
        if (parsed.contains("customRelRect") && parsed["customRelRect"].is_object()) {
            qs["customRelRect"] = parsed["customRelRect"];
        } else {
            qs.erase("customRelRect");
        }
        saveToConfigLocked();
        writeRuntimeConfigAtomicLocked(TraceId::generate());
    }
    LOG_DEBUG("[WindowKit] Persisted Quick Switch v2 custom bounds silently");
    return true;
}

nlohmann::json WindowKitController::startWorkspaceCapture(const nlohmann::json& params) {
    return execAction("start_workspace_capture", params);
}

nlohmann::json WindowKitController::createWorkspaceShortcutJson(
    const std::string& workspaceId,
    const std::string& name) {
    if (workspaceId.empty()) {
        return {{"success", false}, {"error", "workspaceId is required"}};
    }

    std::string resolvedName = name;
    {
        std::lock_guard lock(m_mutex);
        if (resolvedName.empty() && m_settings.contains("workspaces") &&
            m_settings["workspaces"].contains("items") && m_settings["workspaces"]["items"].is_array()) {
            for (const auto& item : m_settings["workspaces"]["items"]) {
                if (item.is_object() && item.value("id", std::string{}) == workspaceId) {
                    resolvedName = item.value("name", std::string{});
                    break;
                }
            }
        }
    }
    if (resolvedName.empty()) {
        resolvedName = workspaceId;
    }

    PWSTR desktopPathRaw = nullptr;
    if (FAILED(SHGetKnownFolderPath(FOLDERID_Desktop, 0, nullptr, &desktopPathRaw)) || !desktopPathRaw) {
        return {{"success", false}, {"error", "failed to resolve user Desktop folder"}};
    }
    std::filesystem::path desktopDir(desktopPathRaw);
    CoTaskMemFree(desktopPathRaw);

    const std::string safeName = sanitizeShortcutFileName(resolvedName);
    const std::filesystem::path lnkPath = desktopDir / (WinUtils::utf8ToWstring(safeName) + L".lnk");
    const std::filesystem::path exePath = WinUtils::getExePath();
    if (exePath.empty()) {
        return {{"success", false}, {"error", "failed to resolve host executable path"}};
    }

    const HRESULT coHr = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    const bool needUninit = SUCCEEDED(coHr);

    IShellLinkW* pLink = nullptr;
    HRESULT hr = CoCreateInstance(CLSID_ShellLink, nullptr, CLSCTX_INPROC_SERVER, IID_IShellLinkW,
                                  reinterpret_cast<void**>(&pLink));
    if (FAILED(hr) || !pLink) {
        if (needUninit) CoUninitialize();
        return {{"success", false}, {"error", "failed to create IShellLinkW COM object"}};
    }

    const std::wstring wId = WinUtils::utf8ToWstring(workspaceId);
    const std::wstring wArgs = L"--launch-workspace \"" + wId + L"\"";
    const std::wstring wDesc = L"Tools3000 Workspace: " + WinUtils::utf8ToWstring(resolvedName);

    pLink->SetPath(exePath.c_str());
    pLink->SetArguments(wArgs.c_str());
    pLink->SetWorkingDirectory(exePath.parent_path().c_str());
    pLink->SetIconLocation(exePath.c_str(), 0);
    pLink->SetDescription(wDesc.c_str());

    IPersistFile* pFile = nullptr;
    hr = pLink->QueryInterface(IID_IPersistFile, reinterpret_cast<void**>(&pFile));
    if (SUCCEEDED(hr) && pFile) {
        hr = pFile->Save(lnkPath.c_str(), TRUE);
        pFile->Release();
    }
    pLink->Release();
    if (needUninit) CoUninitialize();

    if (FAILED(hr)) {
        return {{"success", false}, {"error", "failed to save desktop shortcut .lnk file"}};
    }

    LOG_INFO("[WindowKit] Created desktop workspace shortcut: {} (id={})",
             WinUtils::pathToUtf8(lnkPath), workspaceId);
    return {
        {"success", true},
        {"shortcutPath", WinUtils::pathToUtf8(lnkPath)}
    };
}

nlohmann::json WindowKitController::auditBindingsJson() {
    std::unordered_set<std::string> liveMonitors;
    const auto monRes = getMonitorsJson();
    if (monRes.value("success", false) && monRes.contains("monitors") && monRes["monitors"].is_array()) {
        for (const auto& m : monRes["monitors"]) {
            if (m.is_object()) {
                const std::string id = m.value("id", std::string{});
                if (!id.empty()) liveMonitors.insert(toLowerAsciiCopy(id));
            }
        }
    }
    // 单元测试或离线环境无子进程时，退回 Win32 EnumDisplayMonitors 获取当前屏幕设备名
    if (liveMonitors.empty()) {
        EnumDisplayMonitors(
            nullptr, nullptr,
            [](HMONITOR hMon, HDC, LPRECT, LPARAM lp) -> BOOL {
                auto* set = reinterpret_cast<std::unordered_set<std::string>*>(lp);
                MONITORINFOEXW mi{};
                mi.cbSize = sizeof(mi);
                if (GetMonitorInfoW(hMon, &mi)) {
                    set->insert(toLowerAsciiCopy(WinUtils::wstringToUtf8(mi.szDevice)));
                }
                return TRUE;
            },
            reinterpret_cast<LPARAM>(&liveMonitors));
    }

    std::unordered_set<std::string> liveDesktops;
    const auto vdRes = getVirtualDesktopsJson();
    if (vdRes.contains("desktops") && vdRes["desktops"].is_array()) {
        for (const auto& d : vdRes["desktops"]) {
            if (d.is_object()) {
                const std::string id = d.value("id", std::string{});
                if (!id.empty()) liveDesktops.insert(toLowerAsciiCopy(id));
            }
        }
    }

    nlohmann::json offlineMonitors = nlohmann::json::array();
    nlohmann::json removedDesktops = nlohmann::json::array();
    size_t totalBindings = 0;

    {
        std::lock_guard lock(m_mutex);
        if (!m_initialized) {
            loadFromConfigLocked();
            m_initialized = true;
        }
        if (m_settings.contains("snapZones") && m_settings["snapZones"].is_object() &&
            m_settings["snapZones"].contains("monitorBindings") &&
            m_settings["snapZones"]["monitorBindings"].is_object()) {
            for (const auto& [rawKey, val] : m_settings["snapZones"]["monitorBindings"].items()) {
                if (!val.is_string()) continue;
                ++totalBindings;
                std::string monId = rawKey;
                std::string deskId;
                const auto atPos = rawKey.rfind('@');
                if (atPos != std::string::npos && atPos > 0 && atPos + 1 < rawKey.size()) {
                    monId = rawKey.substr(0, atPos);
                    deskId = rawKey.substr(atPos + 1);
                }
                if (!liveMonitors.empty() && !liveMonitors.contains(toLowerAsciiCopy(monId))) {
                    offlineMonitors.push_back({
                        {"key", rawKey},
                        {"monitorId", monId},
                        {"desktopId", deskId},
                        {"layoutId", val.get<std::string>()}
                    });
                }
                if (!deskId.empty() && !liveDesktops.empty() &&
                    !liveDesktops.contains(toLowerAsciiCopy(deskId))) {
                    removedDesktops.push_back({
                        {"key", rawKey},
                        {"monitorId", monId},
                        {"desktopId", deskId},
                        {"layoutId", val.get<std::string>()}
                    });
                }
            }
        }
    }

    return {
        {"success", true},
        {"totalBindings", totalBindings},
        {"offlineMonitorCount", offlineMonitors.size()},
        {"removedDesktopCount", removedDesktops.size()},
        {"offlineMonitorBindings", offlineMonitors},
        {"removedDesktopBindings", removedDesktops}
    };
}

nlohmann::json WindowKitController::cleanStaleBindingsJson(
    bool cleanOfflineMonitors,
    bool cleanRemovedDesktops) {
    const auto audit = auditBindingsJson();
    if (!audit.value("success", false)) {
        return audit;
    }

    std::unordered_set<std::string> keysToRemove;
    if (cleanOfflineMonitors && audit.contains("offlineMonitorBindings")) {
        for (const auto& item : audit["offlineMonitorBindings"]) {
            if (item.is_object() && item.contains("key")) {
                keysToRemove.insert(item["key"].get<std::string>());
            }
        }
    }
    if (cleanRemovedDesktops && audit.contains("removedDesktopBindings")) {
        for (const auto& item : audit["removedDesktopBindings"]) {
            if (item.is_object() && item.contains("key")) {
                keysToRemove.insert(item["key"].get<std::string>());
            }
        }
    }

    size_t removedCount = 0;
    if (!keysToRemove.empty()) {
        std::lock_guard lock(m_mutex);
        if (m_settings.contains("snapZones") && m_settings["snapZones"].is_object() &&
            m_settings["snapZones"].contains("monitorBindings") &&
            m_settings["snapZones"]["monitorBindings"].is_object()) {
            auto& bindings = m_settings["snapZones"]["monitorBindings"];
            for (const auto& k : keysToRemove) {
                if (bindings.contains(k)) {
                    bindings.erase(k);
                    ++removedCount;
                }
            }
            if (removedCount > 0) {
                saveToConfigLocked();
                reconcileProcessStateLocked(false);
            }
        }
    }

    if (removedCount > 0) {
        pushStateChangedEvents();
        LOG_INFO("[WindowKit] Cleaned {} stale monitor/desktop layout binding(s)", removedCount);
    }
    return {
        {"success", true},
        {"removedCount", removedCount}
    };
}

nlohmann::json WindowKitController::previewBorderStyle(const nlohmann::json& params) {
    return execAction("preview_border_style", params);
}

nlohmann::json WindowKitController::launchWorkspaceById(const std::string& workspaceId) {
    if (workspaceId.empty()) {
        return {{"success", false}, {"error", "workspaceId is required"}};
    }
    // 允许从桌面快捷方式冷启动已保存的工作区：若当前未启子进程，确保 master & workspaces 有效后拉起
    {
        std::lock_guard lock(m_mutex);
        if (!m_initialized) {
            loadFromConfigLocked();
            m_initialized = true;
        }
        if (!m_masterEnabled || !m_settings["features"].value("workspaces", false)) {
            m_masterEnabled = true;
            m_settings["features"]["workspaces"] = true;
            saveToConfigLocked();
            reconcileProcessStateLocked(true);
        }
    }
    return execAction("launch_workspace", {{"workspaceId", workspaceId}});
}

nlohmann::json WindowKitController::captureWorkspaceJson(const std::string& name) {
    const auto outFile = WinUtils::getConfigDirectory() /
        (L"wk_capture_" + std::to_wstring(GetTickCount64()) + L".json");
    const auto res = runBoundedOneShotCli(L"--capture-workspace \"" + outFile.wstring() + L"\"", outFile, 4500);
    if (!res.value("success", false)) {
        return res;
    }
    nlohmann::json apps = nlohmann::json::array();
    const auto& data = res["data"];
    const auto& rawApps = (data.is_object() && data.contains("apps") && data["apps"].is_array())
        ? data["apps"]
        : (data.is_object() && data.contains("windows") && data["windows"].is_array()
               ? data["windows"]
               : nlohmann::json::array());
    size_t seq = 0;
    for (const auto& w : rawApps) {
        if (!w.is_object()) continue;
        apps.push_back(normalizeWorkspaceAppForUi(w, seq++));
    }

    const std::string wsId = "ws-" + std::to_string(GetTickCount64());
    const std::string wsName = name.empty() ? ("Workspace " + std::to_string(GetTickCount64() % 1000)) : name;
    return {
        {"success", true},
        {"monitors", data.is_object() ? data.value("monitors", nlohmann::json::array()) : nlohmann::json::array()},
        {"workspace", {
            {"id", wsId},
            {"name", wsName},
            {"boundLayoutId", data.is_object() ? data.value("boundLayoutId", std::string{}) : std::string{}},
            {"desktopId", data.is_object() ? data.value("desktopId", std::string{}) : std::string{}},
            {"desktopName", data.is_object() ? data.value("desktopName", std::string{}) : std::string{}},
            {"moveExistingWindows", true},
            {"unrelatedAction", "keep"},
            {"createDesktopShortcut", false},
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
