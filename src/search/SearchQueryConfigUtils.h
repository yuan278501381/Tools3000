#pragma once

#include "core/config/ConfigManager.h"
#include "core/i18n/UiI18n.h"

#include <windows.h>
#include <nlohmann/json.hpp>
#include <algorithm>
#include <cctype>
#include <cstddef>
#include <string>
#include <string_view>
#include <unordered_set>
#include <vector>

namespace tools3000::search {

/// 根据有效界面语言判定未显式配置时的默认拼音搜索开关：
/// - 中文语境 (zh-CN / zh-TW) 默认开启，开箱支持汉语拼音首字母与全拼；
/// - 非中文语境 (en-US / ja-JP / ko-KR / ru-RU / pt-BR / es-ES) 默认关闭，彻底消除日文汉字 (Kanji) 等假阳性碰撞。
inline constexpr bool defaultPinyinEnabledForUiLanguage(tools3000::core::UiLanguage lang) noexcept {
    return lang == tools3000::core::UiLanguage::ZhCN ||
           lang == tools3000::core::UiLanguage::ZhTW;
}

/// 解析当前生效的拼音搜索开关：用户显式保存的 `/search/pinyinEnabled` 始终优先，未配置时按有效语言自适应。
inline bool resolveEffectivePinyinEnabled(
    const tools3000::core::ConfigManager& cfg,
    tools3000::core::UiLanguage effectiveUiLang = tools3000::core::UiI18n::getEffectiveLanguage()) {
    return cfg.get<bool>("/search/pinyinEnabled", defaultPinyinEnabledForUiLanguage(effectiveUiLang));
}

inline std::string normalizeExtensionToken(std::string_view raw) {
    while (!raw.empty() && (raw.front() == '.' || std::isspace(static_cast<unsigned char>(raw.front())))) {
        raw.remove_prefix(1);
    }
    while (!raw.empty() && std::isspace(static_cast<unsigned char>(raw.back()))) {
        raw.remove_suffix(1);
    }
    std::string lowered;
    lowered.reserve(raw.size());
    for (unsigned char ch : raw) {
        lowered.push_back(static_cast<char>(std::tolower(ch)));
    }
    return lowered;
}

inline std::vector<std::string> mergeDisabledContentExtensions(
    const nlohmann::json& normalizedParams,
    const std::vector<std::string>& configDisabledExts,
    std::size_t maxItems) {
    std::vector<std::string> merged;
    std::unordered_set<std::string> seen;

    const auto appendExt = [&](std::string_view extView) {
        if (merged.size() >= maxItems) return;
        std::string clean = normalizeExtensionToken(extView);
        if (clean.empty()) return;
        if (seen.insert(clean).second) {
            merged.push_back(std::move(clean));
        }
    };

    if (const auto it = normalizedParams.find("contentDisabledExts");
        it != normalizedParams.end() && it->is_array()) {
        for (const auto& item : *it) {
            if (item.is_string()) {
                appendExt(item.get_ref<const std::string&>());
            }
        }
    }

    for (const auto& ext : configDisabledExts) {
        appendExt(ext);
    }

    return merged;
}

inline bool hasExplicitContentQuerySyntax(std::string_view query) {
    std::size_t i = 0;
    const std::size_t n = query.size();
    while (i < n) {
        while (i < n && (std::isspace(static_cast<unsigned char>(query[i])) || query[i] == '|')) {
            ++i;
        }
        if (i >= n) break;

        const std::size_t tokenStart = i;
        bool inQuotes = false;
        while (i < n) {
            const char ch = query[i];
            if (ch == '"') {
                inQuotes = !inQuotes;
                ++i;
                continue;
            }
            if (!inQuotes && (std::isspace(static_cast<unsigned char>(ch)) || ch == '|')) {
                break;
            }
            ++i;
        }

        std::string_view token = query.substr(tokenStart, i - tokenStart);
        if (!token.empty() && token.front() == '!') {
            token.remove_prefix(1);
        }
        if (token.empty()) continue;

        auto startsWithIgnoreCaseAscii = [](std::string_view text, std::string_view prefix) {
            if (text.size() < prefix.size()) return false;
            for (std::size_t k = 0; k < prefix.size(); ++k) {
                if (std::tolower(static_cast<unsigned char>(text[k])) !=
                    std::tolower(static_cast<unsigned char>(prefix[k]))) {
                    return false;
                }
            }
            return true;
        };

        if (startsWithIgnoreCaseAscii(token, "content:")) {
            return true;
        }
        // UTF-8 "内容:" -> "\xE5\x86\x85\xE5\xAE\xB9:"
        if (token.starts_with("\xE5\x86\x85\xE5\xAE\xB9:")) {
            return true;
        }
        if (startsWithIgnoreCaseAscii(token, "c:") && token.size() > 2 &&
            token[2] != '\\' && token[2] != '/') {
            return true;
        }
    }
    return false;
}

inline constexpr bool isPipeTransientBusyError(DWORD pipeError) noexcept {
    return pipeError == ERROR_PIPE_BUSY || pipeError == ERROR_SEM_TIMEOUT;
}

}  // namespace tools3000::search
