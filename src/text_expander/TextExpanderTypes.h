#pragma once
// -----------------------------------------------------------------------------
// TextExpanderTypes.h -- 文本扩展器核心数据类型、默认安全规则与 JSON 契约
//
// 纯标准 C++20 头文件（无 Win32 依赖），供匹配引擎、管理器与单元测试共享。
// -----------------------------------------------------------------------------

#ifndef TOOLS3000_TEXT_EXPANDER_TYPES_H
#define TOOLS3000_TEXT_EXPANDER_TYPES_H

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include <nlohmann/json.hpp>

namespace tools3000::text_expander {

/// 短语触发模式
enum class TriggerMode : int {
    Immediate = 0,  // 敲完触发词最后一个字符立刻展开
    Delimiter = 1,  // 敲完触发词后再按分隔符（Space / Tab / Enter）才展开
};

/// 单条文本扩展规则
struct TextExpanderRule {
    std::string id;
    std::string trigger;
    std::string replacement;
    std::string label;
    bool enabled = true;
    TriggerMode mode = TriggerMode::Immediate;
    bool caseSensitive = false;
    uint32_t hitCount = 0;
};

/// 文本扩展器模块配置（默认开启开箱即用；关闭后即刻卸载钩子，零常驻内存）
struct TextExpanderSettings {
    bool enabled = true;
    bool autoExcludeSensitiveApps = true;
    bool restoreClipboard = true;
    std::vector<std::string> excludedProcesses;
};

/// 内置敏感进程排除名单（密码管理器、远程桌面、系统凭据与提权对话）
inline const std::vector<std::string>& defaultSensitiveProcesses() {
    static const std::vector<std::string> kProcesses = {
        "keepass.exe",
        "keepassxc.exe",
        "1password.exe",
        "bitwarden.exe",
        "enpass.exe",
        "lastpass.exe",
        "roboform.exe",
        "dashlane.exe",
        "mstsc.exe",
        "credentialuibroker.exe",
        "consent.exe",
    };
    return kProcesses;
}

/// 首次初始化开箱示例规则（安全、简短、立即可在沙盒验证）
inline std::vector<TextExpanderRule> defaultSampleRules() {
    return {
        TextExpanderRule{
            "sample_date",
            ";date",
            "{{date}}",
            "Current Date",
            true,
            TriggerMode::Immediate,
            false,
            0,
        },
        TextExpanderRule{
            "sample_now",
            ";now",
            "{{datetime}}",
            "Current Date & Time",
            true,
            TriggerMode::Immediate,
            false,
            0,
        },
        TextExpanderRule{
            "sample_thx",
            ";thx",
            "Thanks for reaching out!",
            "Quick Thanks",
            true,
            TriggerMode::Immediate,
            false,
            0,
        },
    };
}

inline std::string toLowerAscii(std::string_view input) {
    std::string out(input);
    std::transform(out.begin(), out.end(), out.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    return out;
}

inline std::string trimAscii(std::string_view input) {
    size_t start = 0;
    while (start < input.size() && std::isspace(static_cast<unsigned char>(input[start]))) {
        ++start;
    }
    size_t end = input.size();
    while (end > start && std::isspace(static_cast<unsigned char>(input[end - 1]))) {
        --end;
    }
    return std::string(input.substr(start, end - start));
}

inline const char* triggerModeToString(TriggerMode mode) noexcept {
    return (mode == TriggerMode::Delimiter) ? "delimiter" : "immediate";
}

inline TriggerMode parseTriggerMode(const nlohmann::json& j, TriggerMode fallback = TriggerMode::Immediate) {
    if (j.is_string()) {
        const std::string lower = toLowerAscii(j.get<std::string>());
        if (lower == "delimiter" || lower == "boundary" || lower == "1") {
            return TriggerMode::Delimiter;
        }
        if (lower == "immediate" || lower == "instant" || lower == "0") {
            return TriggerMode::Immediate;
        }
    } else if (j.is_number_integer()) {
        return (j.get<int>() == static_cast<int>(TriggerMode::Delimiter))
            ? TriggerMode::Delimiter
            : TriggerMode::Immediate;
    }
    return fallback;
}

inline nlohmann::json ruleToJson(const TextExpanderRule& rule) {
    return nlohmann::json{
        {"id", rule.id},
        {"trigger", rule.trigger},
        {"replacement", rule.replacement},
        {"label", rule.label},
        {"enabled", rule.enabled},
        {"mode", triggerModeToString(rule.mode)},
        {"caseSensitive", rule.caseSensitive},
        {"hitCount", rule.hitCount},
    };
}

inline bool ruleFromJson(const nlohmann::json& j, TextExpanderRule& out) {
    if (!j.is_object()) return false;
    if (!j.contains("trigger") || !j["trigger"].is_string()) return false;
    if (!j.contains("replacement") || !j["replacement"].is_string()) return false;

    TextExpanderRule parsed;
    parsed.id = j.value("id", std::string{});
    parsed.trigger = j["trigger"].get<std::string>();
    parsed.replacement = j["replacement"].get<std::string>();
    parsed.label = j.value("label", std::string{});
    parsed.enabled = j.value("enabled", true);
    parsed.mode = j.contains("mode") ? parseTriggerMode(j["mode"]) : TriggerMode::Immediate;
    parsed.caseSensitive = j.value("caseSensitive", false);
    if (j.contains("hitCount") && j["hitCount"].is_number_unsigned()) {
        parsed.hitCount = j["hitCount"].get<uint32_t>();
    } else if (j.contains("hitCount") && j["hitCount"].is_number_integer()) {
        const int v = j["hitCount"].get<int>();
        parsed.hitCount = v > 0 ? static_cast<uint32_t>(v) : 0u;
    }

    if (parsed.trigger.empty()) return false;
    out = std::move(parsed);
    return true;
}

inline std::vector<std::string> normalizeProcessList(const std::vector<std::string>& raw) {
    std::vector<std::string> out;
    out.reserve(raw.size());
    for (const auto& entry : raw) {
        std::string cleaned = toLowerAscii(trimAscii(entry));
        if (cleaned.empty()) continue;
        if (std::find(out.begin(), out.end(), cleaned) == out.end()) {
            out.push_back(std::move(cleaned));
        }
    }
    return out;
}

inline std::vector<std::string> parseExcludedProcessString(std::string_view csv) {
    std::vector<std::string> items;
    size_t pos = 0;
    while (pos <= csv.size()) {
        const size_t sep = csv.find_first_of(",;\n\r", pos);
        const std::string_view part = (sep == std::string_view::npos)
            ? csv.substr(pos)
            : csv.substr(pos, sep - pos);
        std::string cleaned = toLowerAscii(trimAscii(part));
        if (!cleaned.empty()) {
            items.push_back(std::move(cleaned));
        }
        if (sep == std::string_view::npos) break;
        pos = sep + 1;
    }
    return normalizeProcessList(items);
}

}  // namespace tools3000::text_expander

#endif  // TOOLS3000_TEXT_EXPANDER_TYPES_H
