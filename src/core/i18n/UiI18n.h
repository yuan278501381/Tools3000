#pragma once
// ─────────────────────────────────────────────────────────────────────────────
// UiI18n.h — Tools3000 原生 C++ 界面 8 语种国际化单一事实源 (SSOT)
//
// 支持语种:
//   - zh-CN (简体中文)      - zh-TW (繁體中文)      - en-US (English / 全局兜底)
//   - ja-JP (日本語)        - ko-KR (한국어)        - ru-RU (Русский)
//   - pt-BR (Português BR)  - es-ES (Español)
// 默认遵循 /general/language = "auto"（严格跟随 Windows 系统显示语言），并支持
// 运行期手动切换即时生效（无需重启）。
// ─────────────────────────────────────────────────────────────────────────────

#ifndef TOOLS3000_CORE_I18N_UII18N_H
#define TOOLS3000_CORE_I18N_UII18N_H

#include "core/utils/Export.h"

#include <cstdint>
#include <filesystem>
#include <initializer_list>
#include <string>
#include <string_view>

namespace tools3000::core {

enum class UiLanguage : uint8_t {
    ZhCN = 0,
    ZhTW = 1,
    EnUS = 2,
    JaJP = 3,
    KoKR = 4,
    RuRU = 5,
    PtBR = 6,
    EsES = 7,
};

class TOOLS3000CORE_API UiI18n {
public:
    /// 判断配置值是否表示「跟随系统语言」("", "auto", "system")
    static bool isFollowSystemSetting(std::string_view rawSetting) noexcept;

    /// 判断配置值是否为合法的 /general/language 选项（含 "auto"/"system" 与全部受支持语种）
    static bool isSupportedLanguageSetting(std::string_view rawSetting) noexcept;

    /// 将任意 BCP-47 语言标签或简写归一化为受支持的 UiLanguage（未知语种回退 EnUS）
    static UiLanguage parseLanguageTag(std::string_view tag) noexcept;

    /// 将 UiLanguage 转换为规范 BCP-47 标签 ("zh-CN", "zh-TW", "en-US", "ja-JP", "ko-KR", "ru-RU", "pt-BR", "es-ES")
    static const char* toCanonicalTag(UiLanguage lang) noexcept;

    /// 将 UiLanguage 转换为宽字符 BCP-47 区域名（供 DirectWrite CreateTextFormat 本地化字形整形使用）
    static const wchar_t* toLocaleNameW(UiLanguage lang) noexcept;

    /// 探测当前 Windows 系统显示语言并映射至受支持的 UiLanguage（未知语种回退 EnUS）
    static UiLanguage detectSystemLanguage();

    /// 返回当前 Windows 系统显示语言映射后的规范 BCP-47 标签
    static std::string getSystemLanguageTag();

    /// 解析当前有效界面语言（读取 /general/language，默认 "auto" 跟随系统）
    static UiLanguage getEffectiveLanguage();

    /// 返回当前有效界面语言的规范 BCP-47 标签
    static std::string getEffectiveLanguageTag();

    /// 更新运行期缓存的语言配置（由 main 启动与 MessageBridge 收到语言切换后调用）
    static void setConfiguredLanguage(std::string_view rawSetting);

    /// 预热并重新加载当前语种的原生词典（resources/locales/native/<tag>.json）
    static void reloadCurrentCatalog();

    /// 翻译宽字符原生 UI 文案（zh-CN 直返 zh，en-US 直返 en，其余 6 语种 O(1) 查表，缺失时安全回退 en）
    static std::wstring trW(std::wstring_view zh, std::wstring_view en);

    /// 翻译宽字符原生 UI 文案并返回稳定只读指针（生命周期与进程一致，专供返回 const wchar_t* 的辅助函数）
    static const wchar_t* trWCstr(const wchar_t* zh, const wchar_t* en) noexcept;

    /// 翻译 UTF-8 窄字符原生 UI 文案
    static std::string trUtf8(std::string_view zh, std::string_view en);

    /// 翻译 UTF-8 窄字符原生 UI 文案并返回稳定只读指针（生命周期与进程一致）
    static const char* trUtf8Cstr(const char* zh, const char* en) noexcept;

    /// 兼容显式 bool isZh 参量的宽字符翻译（zh-CN/en-US 尊重显式参量，其余 6 语种走原生词典查表）
    static const wchar_t* trWCstrFor(bool preferZhWhenDefault, const wchar_t* zh, const wchar_t* en) noexcept;

    /// 兼容显式 bool isZh 参量的 UTF-8 翻译
    static const char* trUtf8CstrFor(bool preferZhWhenDefault, const char* zh, const char* en) noexcept;

    /// 翻译含位置占位符 ({0}, {1}, ...) 的宽字符原生 UI 模板（永不抛出格式化异常，支持日/韩/俄等语序重排）
    static std::wstring trFmtW(std::wstring_view zhFmt, std::wstring_view enFmt,
                               std::initializer_list<std::wstring_view> args);

    /// 翻译含位置占位符 ({0}, {1}, ...) 的 UTF-8 原生 UI 模板
    static std::string trFmtUtf8(std::string_view zhFmt, std::string_view enFmt,
                                 std::initializer_list<std::string_view> args);

    /// 供自动化测试覆盖运行期语言（空串表示清除覆盖，恢复读取 ConfigManager）
    static void setLanguageOverrideForTest(std::string_view rawSetting);

    /// 供自动化测试指定自定义词典目录（空路径表示恢复默认 EXE 相对路径探测）
    static void setCatalogDirectoryForTest(const std::filesystem::path& dir);

    /// 查询指定语种的原生词典是否成功加载以及已载入词条数（供单测门禁断言）
    static std::size_t getLoadedEntryCount(UiLanguage lang);
};

/// 便捷全局翻译函数（委托至 UiI18n 单一事实源）
TOOLS3000CORE_API std::wstring trW(std::wstring_view zh, std::wstring_view en);
TOOLS3000CORE_API const wchar_t* trWCstr(const wchar_t* zh, const wchar_t* en) noexcept;
TOOLS3000CORE_API std::string trUtf8(std::string_view zh, std::string_view en);
TOOLS3000CORE_API const char* trUtf8Cstr(const char* zh, const char* en) noexcept;
TOOLS3000CORE_API const wchar_t* trWCstrFor(bool preferZhWhenDefault, const wchar_t* zh, const wchar_t* en) noexcept;
TOOLS3000CORE_API const char* trUtf8CstrFor(bool preferZhWhenDefault, const char* zh, const char* en) noexcept;
TOOLS3000CORE_API std::wstring trFmtW(std::wstring_view zhFmt, std::wstring_view enFmt,
                                      std::initializer_list<std::wstring_view> args);
TOOLS3000CORE_API std::string trFmtUtf8(std::string_view zhFmt, std::string_view enFmt,
                                        std::initializer_list<std::string_view> args);

}  // namespace tools3000::core

#endif  // TOOLS3000_CORE_I18N_UII18N_H
