#include "core/i18n/UiI18n.h"

#include "core/config/ConfigManager.h"
#include "core/utils/WinUtils.h"

#include <windows.h>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <cctype>
#include <fstream>
#include <memory>
#include <mutex>
#include <shared_mutex>
#include <unordered_map>

namespace tools3000::core {

namespace {

struct NativeCatalog {
    bool loaded = false;
    std::unordered_map<std::string, std::string> utf8ByKey;
    std::unordered_map<std::string, std::wstring> wideByKey;
};

struct I18nState {
    std::shared_mutex mutex;
    std::string testOverrideLang;
    bool hasTestOverride = false;
    std::filesystem::path testCatalogDir;
    std::array<std::shared_ptr<const NativeCatalog>, 8> catalogs;
};

I18nState& state() {
    static I18nState instance;
    return instance;
}

std::string toLowerAscii(std::string_view sv) {
    std::string out;
    out.reserve(sv.size());
    for (unsigned char ch : sv) {
        if (ch == '_') {
            out.push_back('-');
        } else {
            out.push_back(static_cast<char>(std::tolower(ch)));
        }
    }
    while (!out.empty() && std::isspace(static_cast<unsigned char>(out.front()))) {
        out.erase(out.begin());
    }
    while (!out.empty() && std::isspace(static_cast<unsigned char>(out.back()))) {
        out.pop_back();
    }
    return out;
}

std::filesystem::path resolveCatalogFilePath(const char* canonicalTag, const std::filesystem::path& testDir) {
    const std::wstring filename = WinUtils::utf8ToWstring(std::string(canonicalTag) + ".json");
    if (!testDir.empty()) {
        const auto p = testDir / filename;
        std::error_code ec;
        if (std::filesystem::is_regular_file(p, ec)) return p;
    }

    const auto exeDir = WinUtils::getExeDirectory();
    const std::array<std::filesystem::path, 5> candidates = {
        exeDir / L"resources" / L"locales" / L"native" / filename,
        exeDir / L"locales" / L"native" / filename,
        exeDir.parent_path() / L"resources" / L"locales" / L"native" / filename,
        exeDir.parent_path().parent_path() / L"resources" / L"locales" / L"native" / filename,
        std::filesystem::current_path() / L"resources" / L"locales" / L"native" / filename,
    };

    for (const auto& candidate : candidates) {
        std::error_code ec;
        if (!candidate.empty() && std::filesystem::is_regular_file(candidate, ec)) {
            return candidate;
        }
    }
    return {};
}

std::shared_ptr<const NativeCatalog> loadCatalogFromDisk(UiLanguage lang, const std::filesystem::path& testDir) {
    auto cat = std::make_shared<NativeCatalog>();
    if (lang == UiLanguage::ZhCN || lang == UiLanguage::EnUS) {
        cat->loaded = true;
        return cat;
    }

    const char* tag = UiI18n::toCanonicalTag(lang);
    const auto filePath = resolveCatalogFilePath(tag, testDir);
    if (filePath.empty()) {
        return cat;
    }

    try {
        std::ifstream ifs(filePath, std::ios::binary);
        if (!ifs.is_open()) {
            return cat;
        }
        const auto parsed = nlohmann::json::parse(ifs, nullptr, true, true);
        if (!parsed.is_object()) {
            return cat;
        }
        cat->utf8ByKey.reserve(parsed.size());
        cat->wideByKey.reserve(parsed.size());
        for (auto it = parsed.begin(); it != parsed.end(); ++it) {
            if (!it.value().is_string()) continue;
            const std::string key = it.key();
            const std::string val = it.value().get<std::string>();
            if (key.empty() || val.empty()) continue;
            cat->utf8ByKey.emplace(key, val);
            cat->wideByKey.emplace(key, WinUtils::utf8ToWstring(val));
        }
        cat->loaded = !cat->utf8ByKey.empty();
    } catch (...) {
        // 词典异常时优雅降级为英文兜底，绝不阻塞原生 UI 渲染
    }
    return cat;
}

std::shared_ptr<const NativeCatalog> getOrLoadCatalog(UiLanguage lang) {
    const std::size_t idx = static_cast<std::size_t>(lang);
    auto& st = state();
    {
        std::shared_lock<std::shared_mutex> rlock(st.mutex);
        if (st.catalogs[idx]) {
            return st.catalogs[idx];
        }
    }

    std::filesystem::path testDir;
    {
        std::shared_lock<std::shared_mutex> rlock(st.mutex);
        testDir = st.testCatalogDir;
    }
    auto loaded = loadCatalogFromDisk(lang, testDir);

    std::unique_lock<std::shared_mutex> wlock(st.mutex);
    if (!st.catalogs[idx]) {
        st.catalogs[idx] = loaded;
    }
    return st.catalogs[idx];
}

std::string buildCompositeKey(std::string_view zhUtf8, std::string_view enUtf8) {
    std::string key;
    key.reserve(zhUtf8.size() + 2 + enUtf8.size());
    key.append(zhUtf8);
    key.append("||");
    key.append(enUtf8);
    return key;
}

}  // namespace

bool UiI18n::isFollowSystemSetting(std::string_view rawSetting) noexcept {
    const std::string lower = toLowerAscii(rawSetting);
    return lower.empty() || lower == "auto" || lower == "system";
}

bool UiI18n::isSupportedLanguageSetting(std::string_view rawSetting) noexcept {
    const std::string lower = toLowerAscii(rawSetting);
    if (lower.empty() || lower == "auto" || lower == "system") return true;
    static constexpr std::array<std::string_view, 17> kSupported = {
        "zh", "zh-cn", "zh-sg", "zh-hans",
        "zh-tw", "zh-hk", "zh-mo", "zh-hant",
        "en", "en-us", "en-gb",
        "ja", "ja-jp",
        "ko", "ko-kr",
        "ru", "ru-ru",
    };
    for (auto item : kSupported) {
        if (lower == item) return true;
    }
    return lower == "pt" || lower == "pt-br" || lower == "pt-pt" ||
           lower == "es" || lower == "es-es" || lower == "es-419" || lower == "es-mx";
}

UiLanguage UiI18n::parseLanguageTag(std::string_view tag) noexcept {
    const std::string lower = toLowerAscii(tag);
    if (lower.rfind("zh", 0) == 0) {
        if (lower.find("tw") != std::string::npos ||
            lower.find("hk") != std::string::npos ||
            lower.find("mo") != std::string::npos ||
            lower.find("hant") != std::string::npos) {
            return UiLanguage::ZhTW;
        }
        return UiLanguage::ZhCN;
    }
    if (lower.rfind("ja", 0) == 0) return UiLanguage::JaJP;
    if (lower.rfind("ko", 0) == 0) return UiLanguage::KoKR;
    if (lower.rfind("ru", 0) == 0) return UiLanguage::RuRU;
    if (lower.rfind("pt", 0) == 0) return UiLanguage::PtBR;
    if (lower.rfind("es", 0) == 0) return UiLanguage::EsES;
    return UiLanguage::EnUS;
}

const char* UiI18n::toCanonicalTag(UiLanguage lang) noexcept {
    switch (lang) {
        case UiLanguage::ZhCN: return "zh-CN";
        case UiLanguage::ZhTW: return "zh-TW";
        case UiLanguage::EnUS: return "en-US";
        case UiLanguage::JaJP: return "ja-JP";
        case UiLanguage::KoKR: return "ko-KR";
        case UiLanguage::RuRU: return "ru-RU";
        case UiLanguage::PtBR: return "pt-BR";
        case UiLanguage::EsES: return "es-ES";
    }
    return "en-US";
}

const wchar_t* UiI18n::toLocaleNameW(UiLanguage lang) noexcept {
    switch (lang) {
        case UiLanguage::ZhCN: return L"zh-CN";
        case UiLanguage::ZhTW: return L"zh-TW";
        case UiLanguage::EnUS: return L"en-US";
        case UiLanguage::JaJP: return L"ja-JP";
        case UiLanguage::KoKR: return L"ko-KR";
        case UiLanguage::RuRU: return L"ru-RU";
        case UiLanguage::PtBR: return L"pt-BR";
        case UiLanguage::EsES: return L"es-ES";
    }
    return L"en-US";
}

UiLanguage UiI18n::detectSystemLanguage() {
    const LANGID langId = GetUserDefaultUILanguage();
    wchar_t localeBuf[LOCALE_NAME_MAX_LENGTH] = {};
    if (LCIDToLocaleName(MAKELCID(langId, SORT_DEFAULT), localeBuf, LOCALE_NAME_MAX_LENGTH, 0) > 0) {
        return parseLanguageTag(WinUtils::wstringToUtf8(localeBuf));
    }

    switch (PRIMARYLANGID(langId)) {
        case LANG_CHINESE: {
            const WORD sub = SUBLANGID(langId);
            if (sub == SUBLANG_CHINESE_TRADITIONAL ||
                sub == SUBLANG_CHINESE_HONGKONG ||
                sub == SUBLANG_CHINESE_MACAU) {
                return UiLanguage::ZhTW;
            }
            return UiLanguage::ZhCN;
        }
        case LANG_JAPANESE: return UiLanguage::JaJP;
        case LANG_KOREAN: return UiLanguage::KoKR;
        case LANG_RUSSIAN: return UiLanguage::RuRU;
        case LANG_PORTUGUESE: return UiLanguage::PtBR;
        case LANG_SPANISH: return UiLanguage::EsES;
        default: return UiLanguage::EnUS;
    }
}

std::string UiI18n::getSystemLanguageTag() {
    return toCanonicalTag(detectSystemLanguage());
}

UiLanguage UiI18n::getEffectiveLanguage() {
    {
        auto& st = state();
        std::shared_lock<std::shared_mutex> rlock(st.mutex);
        if (st.hasTestOverride) {
            if (isFollowSystemSetting(st.testOverrideLang)) {
                return detectSystemLanguage();
            }
            return parseLanguageTag(st.testOverrideLang);
        }
    }

    const std::string configured = ConfigManager::instance().get<std::string>("/general/language", "auto");
    if (isFollowSystemSetting(configured)) {
        return detectSystemLanguage();
    }
    return parseLanguageTag(configured);
}

std::string UiI18n::getEffectiveLanguageTag() {
    return toCanonicalTag(getEffectiveLanguage());
}

void UiI18n::setConfiguredLanguage(std::string_view rawSetting) {
    const UiLanguage target = isFollowSystemSetting(rawSetting)
        ? detectSystemLanguage()
        : parseLanguageTag(rawSetting);
    (void)getOrLoadCatalog(target);
}

void UiI18n::reloadCurrentCatalog() {
    const UiLanguage lang = getEffectiveLanguage();
    const std::size_t idx = static_cast<std::size_t>(lang);
    std::filesystem::path testDir;
    {
        auto& st = state();
        std::shared_lock<std::shared_mutex> rlock(st.mutex);
        testDir = st.testCatalogDir;
    }
    auto loaded = loadCatalogFromDisk(lang, testDir);
    auto& st = state();
    std::unique_lock<std::shared_mutex> wlock(st.mutex);
    st.catalogs[idx] = std::move(loaded);
}

std::wstring UiI18n::trW(std::wstring_view zh, std::wstring_view en) {
    const UiLanguage lang = getEffectiveLanguage();
    if (lang == UiLanguage::ZhCN) {
        return std::wstring(zh);
    }
    if (lang == UiLanguage::EnUS) {
        return std::wstring(en);
    }

    const auto cat = getOrLoadCatalog(lang);
    if (cat && cat->loaded) {
        const std::string enUtf8 = WinUtils::wstringToUtf8(std::wstring(en));
        if (!zh.empty()) {
            const std::string composite = buildCompositeKey(WinUtils::wstringToUtf8(std::wstring(zh)), enUtf8);
            if (const auto it = cat->wideByKey.find(composite); it != cat->wideByKey.end()) {
                return it->second;
            }
        }
        if (const auto it = cat->wideByKey.find(enUtf8); it != cat->wideByKey.end()) {
            return it->second;
        }
    }
    return std::wstring(en);
}

const wchar_t* UiI18n::trWCstr(const wchar_t* zh, const wchar_t* en) noexcept {
    const wchar_t* safeZh = zh ? zh : L"";
    const wchar_t* safeEn = en ? en : L"";
    try {
        const UiLanguage lang = getEffectiveLanguage();
        if (lang == UiLanguage::ZhCN) {
            return safeZh;
        }
        if (lang == UiLanguage::EnUS) {
            return safeEn;
        }

        const auto cat = getOrLoadCatalog(lang);
        if (cat && cat->loaded) {
            const std::string enUtf8 = WinUtils::wstringToUtf8(safeEn);
            if (*safeZh != L'\0') {
                const std::string composite = buildCompositeKey(WinUtils::wstringToUtf8(safeZh), enUtf8);
                if (const auto it = cat->wideByKey.find(composite); it != cat->wideByKey.end()) {
                    return it->second.c_str();
                }
            }
            if (const auto it = cat->wideByKey.find(enUtf8); it != cat->wideByKey.end()) {
                return it->second.c_str();
            }
        }
    } catch (...) {
    }
    return safeEn;
}

std::string UiI18n::trUtf8(std::string_view zh, std::string_view en) {
    const UiLanguage lang = getEffectiveLanguage();
    if (lang == UiLanguage::ZhCN) {
        return std::string(zh);
    }
    if (lang == UiLanguage::EnUS) {
        return std::string(en);
    }

    const auto cat = getOrLoadCatalog(lang);
    if (cat && cat->loaded) {
        if (!zh.empty()) {
            const std::string composite = buildCompositeKey(zh, en);
            if (const auto it = cat->utf8ByKey.find(composite); it != cat->utf8ByKey.end()) {
                return it->second;
            }
        }
        if (const auto it = cat->utf8ByKey.find(std::string(en)); it != cat->utf8ByKey.end()) {
            return it->second;
        }
    }
    return std::string(en);
}

const char* UiI18n::trUtf8Cstr(const char* zh, const char* en) noexcept {
    const char* safeZh = zh ? zh : "";
    const char* safeEn = en ? en : "";
    try {
        const UiLanguage lang = getEffectiveLanguage();
        if (lang == UiLanguage::ZhCN) {
            return safeZh;
        }
        if (lang == UiLanguage::EnUS) {
            return safeEn;
        }

        const auto cat = getOrLoadCatalog(lang);
        if (cat && cat->loaded) {
            if (*safeZh != '\0') {
                const std::string composite = buildCompositeKey(safeZh, safeEn);
                if (const auto it = cat->utf8ByKey.find(composite); it != cat->utf8ByKey.end()) {
                    return it->second.c_str();
                }
            }
            if (const auto it = cat->utf8ByKey.find(safeEn); it != cat->utf8ByKey.end()) {
                return it->second.c_str();
            }
        }
    } catch (...) {
    }
    return safeEn;
}

const wchar_t* UiI18n::trWCstrFor(bool preferZhWhenDefault, const wchar_t* zh, const wchar_t* en) noexcept {
    const wchar_t* safeZh = zh ? zh : L"";
    const wchar_t* safeEn = en ? en : L"";
    try {
        const UiLanguage lang = getEffectiveLanguage();
        if (lang == UiLanguage::ZhCN || lang == UiLanguage::EnUS) {
            return preferZhWhenDefault ? safeZh : safeEn;
        }
        return trWCstr(safeZh, safeEn);
    } catch (...) {
    }
    return preferZhWhenDefault ? safeZh : safeEn;
}

const char* UiI18n::trUtf8CstrFor(bool preferZhWhenDefault, const char* zh, const char* en) noexcept {
    const char* safeZh = zh ? zh : "";
    const char* safeEn = en ? en : "";
    try {
        const UiLanguage lang = getEffectiveLanguage();
        if (lang == UiLanguage::ZhCN || lang == UiLanguage::EnUS) {
            return preferZhWhenDefault ? safeZh : safeEn;
        }
        return trUtf8Cstr(safeZh, safeEn);
    } catch (...) {
    }
    return preferZhWhenDefault ? safeZh : safeEn;
}

std::wstring UiI18n::trFmtW(std::wstring_view zhFmt, std::wstring_view enFmt,
                            std::initializer_list<std::wstring_view> args) {
    std::wstring result = trW(zhFmt, enFmt);
    std::size_t idx = 0;
    for (std::wstring_view arg : args) {
        const std::wstring token = L"{" + std::to_wstring(idx) + L"}";
        std::size_t pos = 0;
        while ((pos = result.find(token, pos)) != std::wstring::npos) {
            result.replace(pos, token.size(), arg);
            pos += arg.size();
        }
        ++idx;
    }
    return result;
}

std::string UiI18n::trFmtUtf8(std::string_view zhFmt, std::string_view enFmt,
                              std::initializer_list<std::string_view> args) {
    std::string result = trUtf8(zhFmt, enFmt);
    std::size_t idx = 0;
    for (std::string_view arg : args) {
        const std::string token = "{" + std::to_string(idx) + "}";
        std::size_t pos = 0;
        while ((pos = result.find(token, pos)) != std::string::npos) {
            result.replace(pos, token.size(), arg);
            pos += arg.size();
        }
        ++idx;
    }
    return result;
}

void UiI18n::setLanguageOverrideForTest(std::string_view rawSetting) {
    auto& st = state();
    std::unique_lock<std::shared_mutex> wlock(st.mutex);
    if (rawSetting.empty()) {
        st.hasTestOverride = false;
        st.testOverrideLang.clear();
    } else {
        st.hasTestOverride = true;
        st.testOverrideLang = std::string(rawSetting);
    }
}

void UiI18n::setCatalogDirectoryForTest(const std::filesystem::path& dir) {
    auto& st = state();
    std::unique_lock<std::shared_mutex> wlock(st.mutex);
    st.testCatalogDir = dir;
    st.catalogs.fill(nullptr);
}

std::size_t UiI18n::getLoadedEntryCount(UiLanguage lang) {
    const auto cat = getOrLoadCatalog(lang);
    return cat ? cat->utf8ByKey.size() : 0;
}

std::wstring trW(std::wstring_view zh, std::wstring_view en) {
    return UiI18n::trW(zh, en);
}

const wchar_t* trWCstr(const wchar_t* zh, const wchar_t* en) noexcept {
    return UiI18n::trWCstr(zh, en);
}

std::string trUtf8(std::string_view zh, std::string_view en) {
    return UiI18n::trUtf8(zh, en);
}

const char* trUtf8Cstr(const char* zh, const char* en) noexcept {
    return UiI18n::trUtf8Cstr(zh, en);
}

const wchar_t* trWCstrFor(bool preferZhWhenDefault, const wchar_t* zh, const wchar_t* en) noexcept {
    return UiI18n::trWCstrFor(preferZhWhenDefault, zh, en);
}

const char* trUtf8CstrFor(bool preferZhWhenDefault, const char* zh, const char* en) noexcept {
    return UiI18n::trUtf8CstrFor(preferZhWhenDefault, zh, en);
}

std::wstring trFmtW(std::wstring_view zhFmt, std::wstring_view enFmt,
                    std::initializer_list<std::wstring_view> args) {
    return UiI18n::trFmtW(zhFmt, enFmt, args);
}

std::string trFmtUtf8(std::string_view zhFmt, std::string_view enFmt,
                      std::initializer_list<std::string_view> args) {
    return UiI18n::trFmtUtf8(zhFmt, enFmt, args);
}

}  // namespace tools3000::core
