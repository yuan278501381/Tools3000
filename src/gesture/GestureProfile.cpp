// ─────────────────────────────────────────────────────────────────────────────
// GestureProfile.cpp — 手势配置集实现
// ─────────────────────────────────────────────────────────────────────────────

#include "gesture/GestureProfile.h"
#include "core/i18n/UiI18n.h"
#include "core/logger/Logger.h"
#include "core/utils/ThemeUtils.h"
#include <unordered_set>
#include <algorithm>
#include <cctype>
#include <string_view>

namespace tools3000::gesture {

namespace {

std::string normalizeCodeKey(std::string_view code) {
    std::string out;
    out.reserve(code.size());
    for (unsigned char ch : code) {
        out.push_back(static_cast<char>(std::toupper(ch)));
    }
    return out;
}

std::pair<std::string, std::vector<std::string>> splitPrefixAndSegments(std::string_view code) {
    const std::string upper = normalizeCodeKey(code);
    const size_t plusPos = upper.rfind('+');
    std::string prefix;
    std::string bare = upper;
    if (plusPos != std::string::npos) {
        prefix = upper.substr(0, plusPos + 1);
        bare = upper.substr(plusPos + 1);
    }
    std::vector<std::string> segs;
    size_t start = 0;
    while (start < bare.size()) {
        const size_t dash = bare.find('-', start);
        const std::string tok = bare.substr(start, dash == std::string::npos ? std::string::npos : dash - start);
        if (!tok.empty()) segs.push_back(tok);
        if (dash == std::string::npos) break;
        start = dash + 1;
    }
    return {std::move(prefix), std::move(segs)};
}

bool isStrictSegmentPrefix(const std::vector<std::string>& shorter,
                           const std::vector<std::string>& longer) noexcept {
    if (shorter.empty() || shorter.size() >= longer.size()) return false;
    for (size_t i = 0; i < shorter.size(); ++i) {
        if (shorter[i] != longer[i]) return false;
    }
    return true;
}

}  // namespace

GestureProfile::GestureProfile(const std::string& name) : m_name(name) {}

void GestureProfile::addMapping(const GestureMapping& mapping) {
    // 如果已存在（大小写不敏感同编码），原位覆盖
    const std::string key = normalizeCodeKey(mapping.gestureCode);
    auto it = m_codeIndex.find(key);
    if (it != m_codeIndex.end()) {
        m_mappings[it->second] = mapping;
        LOG_DEBUG("覆盖手势映射: profile={}, code={}, action={}",
                  m_name, mapping.gestureCode, mapping.action.name);
    } else {
        m_codeIndex[key] = m_mappings.size();
        m_mappings.push_back(mapping);
        LOG_DEBUG("添加手势映射: profile={}, code={}, action={}",
                  m_name, mapping.gestureCode, mapping.action.name);
    }
}

void GestureProfile::removeMapping(const std::string& gestureCode) {
    const std::string key = normalizeCodeKey(gestureCode);
    auto it = m_codeIndex.find(key);
    if (it != m_codeIndex.end()) {
        m_mappings.erase(m_mappings.begin() + static_cast<ptrdiff_t>(it->second));
        rebuildIndex();
    }
}

void GestureProfile::setMappingEnabled(const std::string& gestureCode, bool enabled) {
    const std::string key = normalizeCodeKey(gestureCode);
    auto it = m_codeIndex.find(key);
    if (it != m_codeIndex.end()) {
        m_mappings[it->second].enabled = enabled;
        LOG_DEBUG("设置手势状态: profile={}, code={}, enabled={}", m_name, gestureCode, enabled);
    }
}

std::optional<GestureAction> GestureProfile::findAction(const std::string& gestureCode) const {
    const std::string key = normalizeCodeKey(gestureCode);
    auto it = m_codeIndex.find(key);
    if (it != m_codeIndex.end()) {
        if (!m_mappings[it->second].enabled) {
            LOG_DEBUG("手势已禁用，忽略动作: profile={}, code={}", m_name, gestureCode);
            return std::nullopt;
        }
        return m_mappings[it->second].action;
    }
    return std::nullopt;
}

std::optional<GestureMapping> GestureProfile::findMapping(const std::string& gestureCode) const {
    const std::string key = normalizeCodeKey(gestureCode);
    auto it = m_codeIndex.find(key);
    if (it != m_codeIndex.end()) {
        return m_mappings[it->second];
    }
    return std::nullopt;
}

void GestureProfile::reorderMappings(const std::vector<std::string>& orderedCodes) {
    std::vector<GestureMapping> newMappings;
    newMappings.reserve(m_mappings.size());
    std::unordered_set<std::string> placed;

    for (const auto& code : orderedCodes) {
        const std::string key = normalizeCodeKey(code);
        if (auto it = m_codeIndex.find(key); it != m_codeIndex.end() && placed.insert(key).second) {
            newMappings.push_back(m_mappings[it->second]);
        }
    }

    // 保留未在 orderedCodes 中的已有映射
    for (const auto& m : m_mappings) {
        if (!placed.contains(normalizeCodeKey(m.gestureCode))) {
            newMappings.push_back(m);
        }
    }

    m_mappings = std::move(newMappings);
    rebuildIndex();
}

bool GestureProfile::moveMapping(size_t fromIndex, size_t toIndex) {
    if (fromIndex >= m_mappings.size() || toIndex >= m_mappings.size() || fromIndex == toIndex) {
        return false;
    }
    auto item = m_mappings[fromIndex];
    m_mappings.erase(m_mappings.begin() + static_cast<ptrdiff_t>(fromIndex));
    m_mappings.insert(m_mappings.begin() + static_cast<ptrdiff_t>(toIndex), item);
    rebuildIndex();
    return true;
}

TriggerModeState GestureProfile::getTriggerState(const std::string& triggerKey) const {
    if (auto it = m_triggerStates.find(triggerKey); it != m_triggerStates.end()) {
        return it->second;
    }
    return TriggerModeState::Default;
}

void GestureProfile::setTriggerState(const std::string& triggerKey, TriggerModeState state) {
    if (state == TriggerModeState::Default) {
        m_triggerStates.erase(triggerKey);
    } else {
        m_triggerStates[triggerKey] = state;
    }
    LOG_DEBUG("设置触发方式状态: profile={}, trigger={}, state={}",
              m_name, triggerKey, triggerStateToString(state));
}

void GestureProfile::setAllTriggerStates(TriggerModeState state) {
    static const std::vector<std::string> kKnownTriggers = {
        "right", "middle", "left", "xbutton1", "xbutton2",
        "edge_top_slide", "edge_bottom_slide", "edge_left_slide", "edge_right_slide",
        "edge_top_wheel", "edge_bottom_wheel", "edge_left_wheel", "edge_right_wheel",
        "edge_top_right", "edge_top_middle", "edge_top_left"
    };

    if (state == TriggerModeState::Default) {
        m_triggerStates.clear();
    } else {
        for (const auto& key : kKnownTriggers) {
            m_triggerStates[key] = state;
        }
    }
    LOG_INFO("批量设置触发方式: profile={}, state={}", m_name, triggerStateToString(state));
}

bool GestureProfile::hasGesture(const std::string& gestureCode) const {
    return m_codeIndex.contains(normalizeCodeKey(gestureCode));
}

std::vector<std::string> GestureProfile::detectConflicts(const std::string& gestureCode) const {
    std::vector<std::string> conflicts;
    const std::string targetKey = normalizeCodeKey(gestureCode);
    const auto [targetPrefix, targetSegs] = splitPrefixAndSegments(gestureCode);
    for (const auto& m : m_mappings) {
        const std::string existingKey = normalizeCodeKey(m.gestureCode);
        if (existingKey == targetKey) continue;
        const auto [existingPrefix, existingSegs] = splitPrefixAndSegments(m.gestureCode);
        if (existingPrefix != targetPrefix) continue;
        // 段级前缀冲突检测：仅当同触发前缀且方向段序列严格互为前缀时才冲突（杜绝 D 与 DR-UR 误报）
        if (isStrictSegmentPrefix(targetSegs, existingSegs) ||
            isStrictSegmentPrefix(existingSegs, targetSegs)) {
            conflicts.push_back(m.gestureCode);
        }
    }
    return conflicts;
}

void GestureProfile::rebuildIndex() {
    m_codeIndex.clear();
    for (size_t i = 0; i < m_mappings.size(); ++i) {
        m_codeIndex[normalizeCodeKey(m_mappings[i].gestureCode)] = i;
    }
}

// ── 默认 Profile 工厂 ───────────────────────────────────────────────────────

GestureProfile GestureProfile::createDefaultGlobal() {
    GestureProfile profile("default");
    const bool zh = tools3000::core::isUiLanguageChinese();

    auto addKeys = [&](const std::string& code, const std::string& name,
                       const std::string& keys, const std::string& desc = "") {
        GestureMapping mapping;
        mapping.gestureCode = code;
        mapping.action.type = ActionType::SendKeys;
        mapping.action.name = name;
        mapping.action.description = desc;
        mapping.action.keyStroke = KeyStroke::fromString(keys);
        profile.addMapping(mapping);
    };

    auto addBuiltin = [&](const std::string& code, const std::string& name,
                          BuiltinCommand cmd, const std::string& desc = "",
                          bool enabled = true) {
        GestureMapping mapping;
        mapping.gestureCode = code;
        mapping.enabled = enabled;
        mapping.action.type = ActionType::BuiltinCommand;
        mapping.action.name = name;
        mapping.action.description = desc;
        mapping.action.builtinCmd = cmd;
        profile.addMapping(mapping);
    };

    // 默认手势集 (符合现代浏览器与效率手势直觉，自适应当前应用 UI 语言)
    addKeys("L",        tools3000::core::trUtf8CstrFor(zh, "后退", "Back"),                        "Alt+Left",         tools3000::core::trUtf8CstrFor(zh, "网页/浏览器/文件管理器后退", "Navigate back in browser or file explorer"));
    addKeys("R",        tools3000::core::trUtf8CstrFor(zh, "前进", "Forward"),                     "Alt+Right",        tools3000::core::trUtf8CstrFor(zh, "网页/浏览器/文件管理器前进", "Navigate forward in browser or file explorer"));
    addBuiltin("WheelUp",   tools3000::core::trUtf8CstrFor(zh, "上一个虚拟桌面", "Previous Virtual Desktop"), BuiltinCommand::PrevVirtualDesktop, tools3000::core::trUtf8CstrFor(zh, "按住右键向上滚动滚轮切换至上一个虚拟桌面", "Hold right button and scroll up to switch to previous virtual desktop"));
    addBuiltin("WheelDown", tools3000::core::trUtf8CstrFor(zh, "下一个虚拟桌面", "Next Virtual Desktop"),     BuiltinCommand::NextVirtualDesktop, tools3000::core::trUtf8CstrFor(zh, "按住右键向下滚动滚轮切换至下一个虚拟桌面", "Hold right button and scroll down to switch to next virtual desktop"));
    addKeys("Middle+L", tools3000::core::trUtf8CstrFor(zh, "上一曲", "Previous Track"),            "MediaPrev",        tools3000::core::trUtf8CstrFor(zh, "全局多媒体上一曲 (鼠标中键向左滑动)", "Global media previous track (middle button swipe left)"));
    addKeys("Middle+R", tools3000::core::trUtf8CstrFor(zh, "下一曲", "Next Track"),                "MediaNext",        tools3000::core::trUtf8CstrFor(zh, "全局多媒体下一曲 (鼠标中键向右滑动)", "Global media next track (middle button swipe right)"));
    addBuiltin("U",     tools3000::core::trUtf8CstrFor(zh, "最大化/还原", "Maximize / Restore"),   BuiltinCommand::MaximizeWindow, tools3000::core::trUtf8CstrFor(zh, "最大化或还原当前窗口", "Maximize or restore active window"));
    addBuiltin("D",     tools3000::core::trUtf8CstrFor(zh, "最小化", "Minimize"),                  BuiltinCommand::MinimizeWindow, tools3000::core::trUtf8CstrFor(zh, "最小化当前窗口", "Minimize active window"));
    addKeys("D-R",      tools3000::core::trUtf8CstrFor(zh, "关闭标签页", "Close Tab"),             "Ctrl+W",           tools3000::core::trUtf8CstrFor(zh, "关闭当前标签页或文档", "Close active tab or document"));
    addKeys("R-D",      tools3000::core::trUtf8CstrFor(zh, "恢复关闭的标签页", "Restore Closed Tab"), "Ctrl+Shift+T",  tools3000::core::trUtf8CstrFor(zh, "重新打开最近关闭的标签页 (如 Chrome/Edge 恢复标签)", "Reopen recently closed tab"));
    addKeys("D-L",      tools3000::core::trUtf8CstrFor(zh, "关闭窗口", "Close Window"),            "Alt+F4",           tools3000::core::trUtf8CstrFor(zh, "关闭当前活动窗口或应用程序", "Close active window or application"));
    addKeys("U-R",      tools3000::core::trUtf8CstrFor(zh, "下一个标签页", "Next Tab"),            "Ctrl+Tab",         tools3000::core::trUtf8CstrFor(zh, "切换到下一个标签页", "Switch to next tab"));
    addKeys("U-L",      tools3000::core::trUtf8CstrFor(zh, "上一个标签页", "Previous Tab"),        "Ctrl+Shift+Tab",   tools3000::core::trUtf8CstrFor(zh, "切换到上一个标签页", "Switch to previous tab"));
    addKeys("L-U-R",    tools3000::core::trUtf8CstrFor(zh, "刷新", "Refresh"),                     "F5",               tools3000::core::trUtf8CstrFor(zh, "刷新页面", "Refresh current view"));
    addKeys("U-D",      tools3000::core::trUtf8CstrFor(zh, "新建标签页", "New Tab"),               "Ctrl+T",           tools3000::core::trUtf8CstrFor(zh, "新建标签页", "Open new tab"));
    addKeys("L-D",      tools3000::core::trUtf8CstrFor(zh, "显示桌面", "Show Desktop"),            "Win+D",            tools3000::core::trUtf8CstrFor(zh, "一键显示/隐藏桌面", "Show or hide desktop"));
    addKeys("R-U",      tools3000::core::trUtf8CstrFor(zh, "任务视图", "Task View"),               "Win+Tab",          tools3000::core::trUtf8CstrFor(zh, "打开 Windows 任务视图", "Open Windows Task View"));
    addBuiltin("D-R-D", tools3000::core::trUtf8CstrFor(zh, "屏幕截图", "Take Screenshot"),         BuiltinCommand::TakeScreenshot, tools3000::core::trUtf8CstrFor(zh, "唤起 Tools3000 屏幕截图", "Trigger Tools3000 screenshot"));
    addKeys("L-U",      tools3000::core::trUtf8CstrFor(zh, "剪切", "Cut"),                         "Ctrl+X",           tools3000::core::trUtf8CstrFor(zh, "剪切选中内容", "Cut selected content"));
    addKeys("R-L",      tools3000::core::trUtf8CstrFor(zh, "全选", "Select All"),                  "Ctrl+A",           tools3000::core::trUtf8CstrFor(zh, "全选当前内容", "Select all content"));
    addKeys("L-R",      tools3000::core::trUtf8CstrFor(zh, "撤销", "Undo"),                        "Ctrl+Z",           tools3000::core::trUtf8CstrFor(zh, "撤销上一步操作", "Undo last action"));
    addKeys("U-D-U",    tools3000::core::trUtf8CstrFor(zh, "强制刷新", "Hard Refresh"),            "Ctrl+F5",          tools3000::core::trUtf8CstrFor(zh, "强制刷新忽略缓存", "Hard refresh bypassing cache"));
    addKeys("D-U-D",    tools3000::core::trUtf8CstrFor(zh, "任务管理器", "Task Manager"),          "Ctrl+Shift+Escape",tools3000::core::trUtf8CstrFor(zh, "打开 Windows 任务管理器", "Open Windows Task Manager"));
    // 屏幕左边缘滚轮调节硬件亮度：默认预置但默认关闭，用户在设置页可见且一键拨开即用
    addBuiltin("LeftEdge+WheelUp",   tools3000::core::trUtf8CstrFor(zh, "调高屏幕亮度", "Brightness Up"),   BuiltinCommand::BrightnessUp,   tools3000::core::trUtf8CstrFor(zh, "提高光标所在显示器的硬件亮度", "Increase brightness of the display under cursor"), false);
    addBuiltin("LeftEdge+WheelDown", tools3000::core::trUtf8CstrFor(zh, "调低屏幕亮度", "Brightness Down"), BuiltinCommand::BrightnessDown, tools3000::core::trUtf8CstrFor(zh, "降低光标所在显示器的硬件亮度", "Decrease brightness of the display under cursor"), false);

    LOG_INFO("创建默认全局手势配置集, 手势数量={}", profile.getMappings().size());
    return profile;
}

GestureProfile GestureProfile::createBrowserProfile() {
    GestureProfile profile("browser");
    const bool zh = tools3000::core::isUiLanguageChinese();

    GestureMapping restoreTab;
    restoreTab.gestureCode = "R-D";
    restoreTab.action.type = ActionType::SendKeys;
    restoreTab.action.name = tools3000::core::trUtf8CstrFor(zh, "恢复关闭的标签页", "Restore Closed Tab");
    restoreTab.action.description = tools3000::core::trUtf8CstrFor(zh, "恢复最近关闭的标签页 (浏览器专用)", "Restore recently closed browser tab");
    restoreTab.action.keyStroke = KeyStroke::fromString("Ctrl+Shift+T");
    profile.addMapping(restoreTab);

    GestureMapping paste;
    paste.gestureCode = "D-R";
    paste.action.type = ActionType::SendKeys;
    paste.action.name = tools3000::core::trUtf8CstrFor(zh, "粘贴", "Paste");
    paste.action.description = tools3000::core::trUtf8CstrFor(zh, "粘贴 (浏览器 Profile 中下右映射为粘贴)", "Paste from clipboard");
    paste.action.keyStroke = KeyStroke::fromString("Ctrl+V");
    profile.addMapping(paste);

    LOG_INFO("创建浏览器专用手势配置集, 手势数量={}", profile.getMappings().size());
    return profile;
}

GestureProfile GestureProfile::createDesktopProfile() {
    GestureProfile profile("special_desktop");
    const bool zh = tools3000::core::isUiLanguageChinese();

    auto addKeys = [&](const std::string& code, const std::string& name,
                       const std::string& keys, const std::string& desc = "") {
        GestureMapping mapping;
        mapping.gestureCode = code;
        mapping.action.type = ActionType::SendKeys;
        mapping.action.name = name;
        mapping.action.description = desc;
        mapping.action.keyStroke = KeyStroke::fromString(keys);
        profile.addMapping(mapping);
    };

    auto addBuiltin = [&](const std::string& code, const std::string& name,
                          BuiltinCommand cmd, const std::string& desc = "",
                          bool enabled = true) {
        GestureMapping mapping;
        mapping.gestureCode = code;
        mapping.enabled = enabled;
        mapping.action.type = ActionType::BuiltinCommand;
        mapping.action.name = name;
        mapping.action.description = desc;
        mapping.action.builtinCmd = cmd;
        profile.addMapping(mapping);
    };

    // 桌面高频实用手势预设
    addKeys("U",      tools3000::core::trUtf8CstrFor(zh, "刷新桌面", "Refresh Desktop"),     "F5",               tools3000::core::trUtf8CstrFor(zh, "刷新桌面图标与排列", "Refresh desktop icons and layout"));
    addBuiltin("D-R", tools3000::core::trUtf8CstrFor(zh, "屏幕截图", "Take Screenshot"),     BuiltinCommand::TakeScreenshot, tools3000::core::trUtf8CstrFor(zh, "唤起 Tools3000 屏幕截图", "Trigger Tools3000 screenshot"));
    addKeys("L",      tools3000::core::trUtf8CstrFor(zh, "打开此电脑", "Open File Explorer"),"Win+E",            tools3000::core::trUtf8CstrFor(zh, "快速打开 Windows 资源管理器", "Open Windows File Explorer"));
    addBuiltin("R",   tools3000::core::trUtf8CstrFor(zh, "全局秒搜", "Instant Search"),      BuiltinCommand::ToggleSearch,   tools3000::core::trUtf8CstrFor(zh, "呼出 Tools3000 文件与内容搜索", "Open Tools3000 instant file & content search"));
    addBuiltin("D",   tools3000::core::trUtf8CstrFor(zh, "显示桌面", "Show Desktop"),        BuiltinCommand::ShowDesktop,    tools3000::core::trUtf8CstrFor(zh, "一键最小化所有窗口显示桌面", "Minimize all windows to show desktop"));

    LOG_INFO("创建桌面专属手势配置集, 手势数量={}", profile.getMappings().size());
    return profile;
}

GestureProfile GestureProfile::createTaskbarProfile() {
    GestureProfile profile("special_taskbar");
    const bool zh = tools3000::core::isUiLanguageChinese();

    auto addBuiltin = [&](const std::string& code, const std::string& name,
                          BuiltinCommand cmd, const std::string& desc = "",
                          bool enabled = true) {
        GestureMapping mapping;
        mapping.gestureCode = code;
        mapping.enabled = enabled;
        mapping.action.type = ActionType::BuiltinCommand;
        mapping.action.name = name;
        mapping.action.description = desc;
        mapping.action.builtinCmd = cmd;
        profile.addMapping(mapping);
    };

    // 任务栏高频实用手势预设
    addBuiltin("L",   tools3000::core::trUtf8CstrFor(zh, "上一个虚拟桌面", "Previous Virtual Desktop"), BuiltinCommand::PrevVirtualDesktop, tools3000::core::trUtf8CstrFor(zh, "向左滑动切换至上一个虚拟桌面", "Switch to previous virtual desktop"));
    addBuiltin("R",   tools3000::core::trUtf8CstrFor(zh, "下一个虚拟桌面", "Next Virtual Desktop"),     BuiltinCommand::NextVirtualDesktop, tools3000::core::trUtf8CstrFor(zh, "向右滑动切换至下一个虚拟桌面", "Switch to next virtual desktop"));
    addBuiltin("U",   tools3000::core::trUtf8CstrFor(zh, "任务视图", "Task View"),                      BuiltinCommand::TaskView,           tools3000::core::trUtf8CstrFor(zh, "呼出 Windows 任务视图 / 时间线", "Open Windows Task View"));
    addBuiltin("D",   tools3000::core::trUtf8CstrFor(zh, "显示桌面", "Show Desktop"),                   BuiltinCommand::ShowDesktop,        tools3000::core::trUtf8CstrFor(zh, "快速显示桌面或还原", "Show or restore desktop"));

    LOG_INFO("创建任务栏专属手势配置集, 手势数量={}", profile.getMappings().size());
    return profile;
}

// ── JSON 序列化 ──────────────────────────────────────────────────────────────

nlohmann::json GestureProfile::toJson() const {
    nlohmann::json j;
    j["name"] = m_name;
    j["mappings"] = nlohmann::json::array();
    for (const auto& mapping : m_mappings) {
        j["mappings"].push_back(mapping.toJson());
    }

    j["triggerStates"] = nlohmann::json::object();
    for (const auto& [key, state] : m_triggerStates) {
        j["triggerStates"][key] = triggerStateToString(state);
    }
    return j;
}

GestureProfile GestureProfile::fromJson(const nlohmann::json& j) {
    if (!j.is_object()) {
        return GestureProfile("default");
    }
    const std::string profName = (j.contains("name") && j["name"].is_string())
        ? j["name"].get<std::string>()
        : std::string("default");
    GestureProfile profile(profName);
    if (j.contains("mappings") && j["mappings"].is_array()) {
        for (const auto& item : j["mappings"]) {
            if (item.is_object()) {
                profile.addMapping(GestureMapping::fromJson(item));
            }
        }
    }

    if (j.contains("triggerStates") && j["triggerStates"].is_object()) {
        for (const auto& [key, val] : j["triggerStates"].items()) {
            if (val.is_string()) {
                profile.setTriggerState(key, triggerStateFromString(val.get<std::string>()));
            } else if (val.is_number_integer()) {
                const int clampedState = std::clamp(
                    val.get<int>(),
                    static_cast<int>(TriggerModeState::Default),
                    static_cast<int>(TriggerModeState::Disabled));
                profile.setTriggerState(key, static_cast<TriggerModeState>(clampedState));
            }
        }
    }
    return profile;
}

}  // namespace tools3000::gesture
