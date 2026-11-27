// ─────────────────────────────────────────────────────────────────────────────
// QuickTranslatePopupWindow.cpp — 全局划词与剪贴板轻量原生翻译浮窗实现
// ─────────────────────────────────────────────────────────────────────────────

#include "ui/QuickTranslatePopupWindow.h"

#include "core/config/ConfigManager.h"
#include "core/events/EventBus.h"
#include "core/events/MainThreadDispatcher.h"
#include "core/i18n/UiI18n.h"
#include "core/logger/Logger.h"
#include "core/translate/TranslationService.h"
#include "core/utils/DWriteFontUtils.h"
#include "core/utils/ThemeUtils.h"
#include "core/utils/TraceId.h"
#include "core/utils/UiThreadJoin.h"
#include "core/utils/WinUtils.h"

#include <shellscalingapi.h>
#include <windowsx.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <vector>

#pragma comment(lib, "d2d1.lib")
#pragma comment(lib, "dwrite.lib")
#pragma comment(lib, "shcore.lib")

namespace tools3000::ui {

namespace {

constexpr const wchar_t* kPopupClassName = L"Tools3000_QuickTranslatePopup";
constexpr UINT_PTR kTimerLoadingAnim = 1;
constexpr UINT_PTR kTimerCopyReset = 2;

constexpr const char* kCfgQuickSourceLang = "/translate/quickSourceLang";
constexpr const char* kCfgQuickTargetLang = "/translate/quickTargetLang";

constexpr UINT kMenuIdAuto = 1000;
constexpr UINT kMenuIdLangBase = 1100;
constexpr UINT kMenuIdOpenSettings = 1200;

constexpr float kCardWidthDip = 468.0f;
constexpr float kMinCardHeightDip = 172.0f;
constexpr float kMaxCardHeightDip = 500.0f;
constexpr float kOuterPadDip = 16.0f;
constexpr float kHeaderHeightDip = 44.0f;
constexpr float kFooterHeightDip = 46.0f;
constexpr float kSectionGapDip = 10.0f;
constexpr float kCornerRadiusDip = 12.0f;
constexpr int kCursorOffsetDip = 16;
constexpr int kWorkAreaMarginDip = 12;

struct LangMenuEntry {
    const char* code;
    const wchar_t* zhShort;
    const wchar_t* zhMenu;
    const wchar_t* enShort;
    const wchar_t* enMenu;
};

// 全平台统一 10 语种目录（与 InPlaceTranslateCard.tsx 单一事实源对齐）
constexpr std::array<LangMenuEntry, 10> kSupportedLanguages = {{
    {"zh-CN", L"简中", L"简体中文",           L"ZH-CN", L"Chinese (Simplified)"},
    {"zh-TW", L"繁中", L"繁體中文",           L"ZH-TW", L"Chinese (Traditional)"},
    {"en",    L"英语", L"英语 (English)",     L"EN",    L"English"},
    {"ja",    L"日语", L"日语 (日本語)",       L"JA",    L"Japanese (日本語)"},
    {"ko",    L"韩语", L"韩语 (한국어)",       L"KO",    L"Korean (한국어)"},
    {"fr",    L"法语", L"法语 (Français)",    L"FR",    L"French (Français)"},
    {"de",    L"德语", L"德语 (Deutsch)",     L"DE",    L"German (Deutsch)"},
    {"es",    L"西语", L"西班牙语 (Español)",  L"ES",    L"Spanish (Español)"},
    {"ru",    L"俄语", L"俄语 (Русский)",     L"RU",    L"Russian (Русский)"},
    {"pt-BR", L"葡语", L"葡萄牙语 (Português)", L"PT",   L"Portuguese (Português)"},
}};

const LangMenuEntry* findLangEntry(std::string_view raw) noexcept {
    const std::string canon = tools3000::core::translate::TranslationService::canonicalizeLanguageCode(raw);
    for (const auto& entry : kSupportedLanguages) {
        if (canon == entry.code) {
            return &entry;
        }
    }
    return nullptr;
}

constexpr bool pointInD2DRect(const D2D1_RECT_F& rc, float x, float y) noexcept {
    return x >= rc.left && x <= rc.right && y >= rc.top && y <= rc.bottom;
}

std::string toUpperAscii(std::string_view text) {
    std::string out;
    out.reserve(text.size());
    for (unsigned char ch : text) {
        out.push_back(static_cast<char>(std::toupper(ch)));
    }
    return out;
}

std::string shortLangCode(std::string_view raw) {
    const std::string canon = tools3000::core::translate::TranslationService::canonicalizeLanguageCode(raw);
    if (canon.empty() || canon == "auto") return "AUTO";
    if (canon == "zh-CN") return "ZH";
    if (canon == "zh-TW") return "ZH-TW";
    if (canon == "pt-BR") return "PT";
    const size_t dash = canon.find('-');
    return toUpperAscii(dash == std::string::npos ? std::string_view(canon) : std::string_view(canon).substr(0, dash));
}

std::string formatUserFacingError(const tools3000::core::translate::TranslationBatchResult& res) {
    const bool zh = tools3000::core::isUiLanguageChinese();
    if (res.errorCode == "auth_failed") {
        return zh
            ? "翻译鉴权失败，请检查设置页中的 API Key 或额度。"
            : "Translation authentication failed. Please check your API key or quota in Settings.";
    }
    if (res.errorCode == "rate_limited") {
        return zh
            ? "当前翻译接口触发了频率限制，请稍后重试或切换其他翻译引擎。"
            : "Translation provider rate limit reached. Please retry shortly or switch provider.";
    }
    if (res.errorCode == "missing_api_key") {
        return zh
            ? "当前所选翻译服务尚未配置 API Key，请前往设置页填写或切换至免费通道。"
            : "API key is missing for the selected provider. Configure it in Settings or switch to a free channel.";
    }
    if (res.errorCode == "network") {
        return zh
            ? "网络连接超时或不可达，请检查网络代理后点击重试。"
            : "Network request timed out or unreachable. Check your network/proxy and retry.";
    }
    if (!res.errorDetail.empty()) {
        return (zh ? "翻译请求失败 (" : "Translation failed (") + res.errorDetail + ")";
    }
    return zh ? "翻译服务暂时不可用，请点击重试。" : "Translation service temporarily unavailable. Click Retry.";
}

struct PopupPalette {
    D2D1_COLOR_F bg{};
    D2D1_COLOR_F sourceBoxBg{};
    D2D1_COLOR_F border{};
    D2D1_COLOR_F divider{};
    D2D1_COLOR_F titleText{};
    D2D1_COLOR_F sourceText{};
    D2D1_COLOR_F bodyText{};
    D2D1_COLOR_F mutedText{};
    D2D1_COLOR_F errorText{};
    D2D1_COLOR_F badgeBg{};
    D2D1_COLOR_F badgeBorder{};
    D2D1_COLOR_F badgeText{};
    D2D1_COLOR_F pillBg{};
    D2D1_COLOR_F pillBorder{};
    D2D1_COLOR_F btnBg{};
    D2D1_COLOR_F btnHoverBg{};
    D2D1_COLOR_F accent{};
    D2D1_COLOR_F accentHover{};
    D2D1_COLOR_F accentText{};
    D2D1_COLOR_F scrollTrack{};
    D2D1_COLOR_F scrollThumb{};

    static PopupPalette current() {
        const bool dark = tools3000::core::NativeThemePalette::isEffectiveDarkMode();
        const auto acc = tools3000::core::NativeThemePalette::getEffectiveAccentColorRGB();
        PopupPalette p{};
        p.accent = D2D1::ColorF(acc.r, acc.g, acc.b, 1.0f);
        p.accentHover = D2D1::ColorF(
            (std::min)(1.0f, acc.r * 1.12f + 0.04f),
            (std::min)(1.0f, acc.g * 1.12f + 0.04f),
            (std::min)(1.0f, acc.b * 1.12f + 0.04f),
            1.0f);
        p.accentText = D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.98f);
        if (dark) {
            p.bg          = D2D1::ColorF(24.0f / 255.0f, 27.0f / 255.0f, 36.0f / 255.0f, 1.0f);
            p.sourceBoxBg = D2D1::ColorF(34.0f / 255.0f, 39.0f / 255.0f, 52.0f / 255.0f, 1.0f);
            p.border      = D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.14f);
            p.divider     = D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.09f);
            p.titleText   = D2D1::ColorF(241.0f / 255.0f, 245.0f / 255.0f, 249.0f / 255.0f, 0.96f);
            p.sourceText  = D2D1::ColorF(148.0f / 255.0f, 163.0f / 255.0f, 184.0f / 255.0f, 0.96f);
            p.bodyText    = D2D1::ColorF(248.0f / 255.0f, 250.0f / 255.0f, 252.0f / 255.0f, 0.98f);
            p.mutedText   = D2D1::ColorF(148.0f / 255.0f, 163.0f / 255.0f, 184.0f / 255.0f, 0.86f);
            p.errorText   = D2D1::ColorF(248.0f / 255.0f, 113.0f / 255.0f, 113.0f / 255.0f, 0.98f);
            p.badgeBg     = D2D1::ColorF(acc.r, acc.g, acc.b, 0.20f);
            p.badgeBorder = D2D1::ColorF(acc.r, acc.g, acc.b, 0.42f);
            p.badgeText   = D2D1::ColorF(
                (std::min)(1.0f, acc.r * 0.55f + 0.45f),
                (std::min)(1.0f, acc.g * 0.55f + 0.45f),
                (std::min)(1.0f, acc.b * 0.55f + 0.45f),
                1.0f);
            p.pillBg      = D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.07f);
            p.pillBorder  = D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.14f);
            p.btnBg       = D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.07f);
            p.btnHoverBg  = D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.15f);
            p.scrollTrack = D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.06f);
            p.scrollThumb = D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.26f);
        } else {
            p.bg          = D2D1::ColorF(252.0f / 255.0f, 253.0f / 255.0f, 255.0f / 255.0f, 1.0f);
            p.sourceBoxBg = D2D1::ColorF(241.0f / 255.0f, 245.0f / 255.0f, 249.0f / 255.0f, 1.0f);
            p.border      = D2D1::ColorF(15.0f / 255.0f, 23.0f / 255.0f, 42.0f / 255.0f, 0.14f);
            p.divider     = D2D1::ColorF(15.0f / 255.0f, 23.0f / 255.0f, 42.0f / 255.0f, 0.08f);
            p.titleText   = D2D1::ColorF(15.0f / 255.0f, 23.0f / 255.0f, 42.0f / 255.0f, 0.96f);
            p.sourceText  = D2D1::ColorF(71.0f / 255.0f, 85.0f / 255.0f, 105.0f / 255.0f, 0.95f);
            p.bodyText    = D2D1::ColorF(15.0f / 255.0f, 23.0f / 255.0f, 42.0f / 255.0f, 0.98f);
            p.mutedText   = D2D1::ColorF(100.0f / 255.0f, 116.0f / 255.0f, 139.0f / 255.0f, 0.90f);
            p.errorText   = D2D1::ColorF(220.0f / 255.0f, 38.0f / 255.0f, 38.0f / 255.0f, 0.96f);
            p.badgeBg     = D2D1::ColorF(acc.r, acc.g, acc.b, 0.12f);
            p.badgeBorder = D2D1::ColorF(acc.r, acc.g, acc.b, 0.32f);
            p.badgeText   = D2D1::ColorF(acc.r * 0.85f, acc.g * 0.85f, acc.b * 0.85f, 1.0f);
            p.pillBg      = D2D1::ColorF(15.0f / 255.0f, 23.0f / 255.0f, 42.0f / 255.0f, 0.05f);
            p.pillBorder  = D2D1::ColorF(15.0f / 255.0f, 23.0f / 255.0f, 42.0f / 255.0f, 0.12f);
            p.btnBg       = D2D1::ColorF(15.0f / 255.0f, 23.0f / 255.0f, 42.0f / 255.0f, 0.06f);
            p.btnHoverBg  = D2D1::ColorF(15.0f / 255.0f, 23.0f / 255.0f, 42.0f / 255.0f, 0.12f);
            p.scrollTrack = D2D1::ColorF(15.0f / 255.0f, 23.0f / 255.0f, 42.0f / 255.0f, 0.06f);
            p.scrollThumb = D2D1::ColorF(15.0f / 255.0f, 23.0f / 255.0f, 42.0f / 255.0f, 0.25f);
        }
        return p;
    }
};

}  // namespace

QuickTranslatePopupWindow& QuickTranslatePopupWindow::instance() {
    static QuickTranslatePopupWindow inst;
    return inst;
}

QuickTranslatePopupWindow::~QuickTranslatePopupWindow() {
    shutdown();
}

std::string QuickTranslatePopupWindow::sanitizeAndTrimInput(std::string_view rawUtf8, size_t maxCodePoints) {
    const auto first = rawUtf8.find_first_not_of(" \t\r\n\f\v");
    if (first == std::string_view::npos) {
        return {};
    }
    const auto last = rawUtf8.find_last_not_of(" \t\r\n\f\v");
    std::string trimmed(rawUtf8.substr(first, last - first + 1));

    // 统一换行为 \n，压缩连续多余空行
    std::string normalized;
    normalized.reserve(trimmed.size());
    for (size_t i = 0; i < trimmed.size(); ++i) {
        if (trimmed[i] == '\r') {
            if (i + 1 < trimmed.size() && trimmed[i + 1] == '\n') {
                ++i;
            }
            normalized.push_back('\n');
        } else {
            normalized.push_back(trimmed[i]);
        }
    }

    std::wstring wide = tools3000::core::WinUtils::utf8ToWstring(normalized);
    if (wide.size() > maxCodePoints) {
        size_t cut = maxCodePoints;
        if (cut > 0 && wide[cut - 1] >= 0xD800 && wide[cut - 1] <= 0xDBFF) {
            --cut;
        }
        wide.resize(cut);
        return tools3000::core::WinUtils::wstringToUtf8(wide);
    }
    return normalized;
}

RECT QuickTranslatePopupWindow::computePopupPlacement(
    POINT anchorPt, int popupW, int popupH, const RECT& workArea) noexcept {
    const int safeW = (std::max)(1, popupW);
    const int safeH = (std::max)(1, popupH);
    const int areaW = (std::max)(1, static_cast<int>(workArea.right - workArea.left));
    const int areaH = (std::max)(1, static_cast<int>(workArea.bottom - workArea.top));
    const int clampedW = (std::min)(safeW, areaW);
    const int clampedH = (std::min)(safeH, areaH);

    int left = anchorPt.x + kCursorOffsetDip;
    int top = anchorPt.y + kCursorOffsetDip;

    if (left + clampedW + kWorkAreaMarginDip > workArea.right) {
        left = anchorPt.x - clampedW - kCursorOffsetDip;
    }
    if (top + clampedH + kWorkAreaMarginDip > workArea.bottom) {
        top = anchorPt.y - clampedH - kCursorOffsetDip;
    }

    const int minLeft = workArea.left + kWorkAreaMarginDip;
    const int maxLeft = (std::max)(minLeft, static_cast<int>(workArea.right) - clampedW - kWorkAreaMarginDip);
    const int minTop = workArea.top + kWorkAreaMarginDip;
    const int maxTop = (std::max)(minTop, static_cast<int>(workArea.bottom) - clampedH - kWorkAreaMarginDip);

    left = std::clamp(left, minLeft, maxLeft);
    top = std::clamp(top, minTop, maxTop);
    return RECT{left, top, left + clampedW, top + clampedH};
}

std::string QuickTranslatePopupWindow::formatProviderDisplayLabel(
    const tools3000::core::translate::TranslationSettings& settings,
    std::string_view providerUsed) {
    if (!providerUsed.empty()) {
        if (providerUsed == "edge_free" || providerUsed == "edge") return "Edge Translate";
        if (providerUsed == "google_free" || providerUsed == "google") return "Google Translate";
        if (providerUsed == "deepl") return "DeepL";
        if (providerUsed == "index_translate") return "Index-Translate";
        if (providerUsed == "openai_compatible" || providerUsed == "openai_cache") {
            if (tools3000::core::translate::TranslationService::isKeylessPublicOpenAiUrl(settings.openaiBaseUrl)) {
                return "Index-Translate";
            }
            if (tools3000::core::translate::TranslationService::isLoopbackHttpUrl(settings.openaiBaseUrl)) {
                return settings.openaiModel.empty() ? "Ollama" : settings.openaiModel;
            }
            return settings.openaiModel.empty() ? "OpenAI API" : settings.openaiModel;
        }
    }
    const auto kind = tools3000::core::translate::TranslationService::parseProviderKind(settings.provider);
    if (kind == tools3000::core::translate::TranslationProviderKind::OpenAiCompatible) {
        if (tools3000::core::translate::TranslationService::isKeylessPublicOpenAiUrl(settings.openaiBaseUrl)) {
            return "Index-Translate";
        }
        return settings.openaiModel.empty() ? "OpenAI API" : settings.openaiModel;
    }
    if (kind == tools3000::core::translate::TranslationProviderKind::DeepL) {
        return "DeepL";
    }
    if (kind == tools3000::core::translate::TranslationProviderKind::AutoFree) {
        if (settings.freePrimary == tools3000::core::translate::TranslationConstants::kFreePrimaryGoogle) {
            return "Google Translate";
        }
        return "Edge Translate";
    }
    return "Auto Translate";
}

std::string QuickTranslatePopupWindow::formatDirectionBadge(
    std::string_view detectedLang, std::string_view targetLang) {
    const std::string src = shortLangCode(detectedLang);
    const std::string dst = shortLangCode(targetLang);
    return src + " -> " + dst;
}

std::string QuickTranslatePopupWindow::formatLanguagePillLabel(
    std::string_view langCode,
    bool isChineseUi,
    bool isAutoTarget,
    std::string_view detectedLang) {
    const std::string canon = tools3000::core::translate::TranslationService::canonicalizeLanguageCode(langCode);
    if (canon.empty() || canon == "auto") {
        if (!detectedLang.empty()) {
            if (const auto* det = findLangEntry(detectedLang)) {
                const std::wstring prefix = isChineseUi ? L"自动(" : L"Auto (";
                const std::wstring name = isChineseUi ? det->zhShort : det->enShort;
                return tools3000::core::WinUtils::wstringToUtf8(prefix + name + L")");
            }
        }
        return tools3000::core::WinUtils::wstringToUtf8(isChineseUi ? L"自动检测" : L"Auto");
    }
    if (const auto* entry = findLangEntry(canon)) {
        const std::wstring base = isChineseUi ? entry->zhShort : entry->enShort;
        if (isAutoTarget) {
            const std::wstring prefix = isChineseUi ? L"自动·" : L"Auto · ";
            return tools3000::core::WinUtils::wstringToUtf8(prefix + base);
        }
        return tools3000::core::WinUtils::wstringToUtf8(base);
    }
    return shortLangCode(canon);
}

QuickTranslatePopupWindow::LanguagePair QuickTranslatePopupWindow::computeSwappedLanguagePair(
    std::string_view currentSourcePref,
    std::string_view currentEffectiveTarget,
    std::string_view lastDistinctTarget,
    std::string_view settingsTargetLang,
    std::string_view settingsSecondaryLang,
    std::string_view uiLanguageCode) {
    using tools3000::core::translate::TranslationService;

    const std::string srcCanon = TranslationService::canonicalizeLanguageCode(currentSourcePref);
    std::string curTarget = TranslationService::canonicalizeLanguageCode(currentEffectiveTarget);
    if (curTarget.empty() || curTarget == "auto") {
        curTarget = "zh-CN";
    }

    // 情况 1：用户显式指定了源语种（如 日语 -> 简中），一键精准互换为 简中 -> 日语
    if (!srcCanon.empty() && srcCanon != "auto") {
        if (srcCanon == curTarget) {
            std::string fallback = TranslationService::canonicalizeLanguageCode(settingsSecondaryLang);
            if (fallback.empty() || fallback == "auto" || fallback == curTarget) {
                fallback = (curTarget == "en") ? "zh-CN" : "en";
            }
            return LanguagePair{"auto", fallback};
        }
        return LanguagePair{curTarget, srcCanon};
    }

    // 情况 2：自动检测源语种模式下，一键在最近使用的对偶目标语种或设置的主/副互译对之间切换
    const std::string lastTarget = TranslationService::canonicalizeLanguageCode(lastDistinctTarget);
    if (!lastTarget.empty() && lastTarget != "auto" && lastTarget != curTarget) {
        return LanguagePair{"auto", lastTarget};
    }

    std::string primary = TranslationService::canonicalizeLanguageCode(settingsTargetLang);
    if (primary.empty() || primary == "auto") {
        primary = TranslationService::canonicalizeLanguageCode(uiLanguageCode);
    }
    if (primary.empty() || primary == "auto") {
        primary = "zh-CN";
    }

    std::string secondary = TranslationService::canonicalizeLanguageCode(settingsSecondaryLang);
    if (secondary.empty() || secondary == "auto" || secondary == primary) {
        secondary = (primary == "en") ? "zh-CN" : "en";
    }

    const std::string nextTarget = (curTarget == primary) ? secondary : primary;
    return LanguagePair{"auto", nextTarget};
}

std::string QuickTranslatePopupWindow::nextQuickTargetLanguage(std::string_view currentOverride) noexcept {
    if (currentOverride.empty()) return "zh-CN";
    if (currentOverride == "zh-CN") return "en";
    if (currentOverride == "en") return "ja";
    return {};
}

void QuickTranslatePopupWindow::loadQuickLanguagePrefs(std::string& outSource, std::string& outTarget) const {
    using tools3000::core::translate::TranslationService;
    auto& cfg = tools3000::core::ConfigManager::instance();
    const std::string rawSrc = cfg.get<std::string>(kCfgQuickSourceLang, "auto");
    const std::string rawDst = cfg.get<std::string>(kCfgQuickTargetLang, "auto");
    const std::string canonSrc = TranslationService::canonicalizeLanguageCode(rawSrc);
    const std::string canonDst = TranslationService::canonicalizeLanguageCode(rawDst);
    outSource = (canonSrc.empty() || canonSrc == "auto" || !findLangEntry(canonSrc)) ? "auto" : canonSrc;
    outTarget = (canonDst.empty() || canonDst == "auto" || !findLangEntry(canonDst)) ? "auto" : canonDst;
}

void QuickTranslatePopupWindow::saveQuickLanguagePrefs(
    std::string_view sourcePref, std::string_view targetPref) const {
    auto& cfg = tools3000::core::ConfigManager::instance();
    const std::string srcVal = sourcePref.empty() ? std::string("auto") : std::string(sourcePref);
    const std::string dstVal = targetPref.empty() ? std::string("auto") : std::string(targetPref);
    cfg.mergePatch({
        {"translate", {
            {"quickSourceLang", srcVal},
            {"quickTargetLang", dstVal}
        }}
    }, "/translate");
}

bool QuickTranslatePopupWindow::initialize(HINSTANCE hInstance) {
    m_hInstance = hInstance ? hInstance : GetModuleHandleW(nullptr);
    m_shuttingDown.store(false, std::memory_order_release);

    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = windowProc;
    wc.hInstance = m_hInstance;
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    wc.lpszClassName = kPopupClassName;
    wc.style = CS_HREDRAW | CS_VREDRAW;
    RegisterClassExW(&wc);

    if (!m_d2dFactory) {
        D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, m_d2dFactory.GetAddressOf());
    }
    if (!m_dwriteFactory) {
        DWriteCreateFactory(
            DWRITE_FACTORY_TYPE_SHARED,
            __uuidof(IDWriteFactory),
            reinterpret_cast<IUnknown**>(m_dwriteFactory.GetAddressOf()));
    }

    LOG_INFO("QuickTranslatePopupWindow initialized");
    return m_d2dFactory && m_dwriteFactory;
}

void QuickTranslatePopupWindow::shutdown() {
    m_shuttingDown.store(true, std::memory_order_release);
    {
        std::lock_guard lock(m_workerMutex);
        cancelActiveWorkerLocked();
    }
    if (m_workerThread.joinable()) {
        tools3000::core::joinWorkerWhilePumpingSentMessages(m_workerThread, 1500);
    }

    discardDeviceResources();
    m_badgeFormat.Reset();
    m_sourceFormat.Reset();
    m_bodyFormat.Reset();
    m_btnFormat.Reset();
    m_sourceLayout.Reset();
    m_bodyLayout.Reset();

    if (HWND hwnd = m_hwnd.exchange(nullptr, std::memory_order_acq_rel)) {
        KillTimer(hwnd, kTimerLoadingAnim);
        KillTimer(hwnd, kTimerCopyReset);
        DestroyWindow(hwnd);
    }
    if (m_helperOwnerHwnd) {
        DestroyWindow(m_helperOwnerHwnd);
        m_helperOwnerHwnd = nullptr;
    }
    m_dwriteFactory.Reset();
    m_d2dFactory.Reset();
    m_state.store(PopupState::Hidden, std::memory_order_release);
}

void QuickTranslatePopupWindow::cancelActiveWorkerLocked() {
    if (m_activeCancelFlag) {
        m_activeCancelFlag->store(true, std::memory_order_release);
        m_activeCancelFlag.reset();
    }
}

void QuickTranslatePopupWindow::triggerQuickTranslate(
    POINT cursorPt, HWND foregroundHwnd, const std::string& hotkeyDisplay) {
    if (m_shuttingDown.load(std::memory_order_acquire)) {
        return;
    }

    const uint64_t reqId = m_requestSeq.fetch_add(1, std::memory_order_acq_rel) + 1;
    auto cancelFlag = std::make_shared<std::atomic<bool>>(false);
    const std::string traceId = tools3000::core::TraceId::current();

    LOG_INFO("Global selection/clipboard translate triggered: reqId={}, hotkey={}", reqId, hotkeyDisplay);

    std::jthread prevWorker;
    {
        std::lock_guard lock(m_workerMutex);
        cancelActiveWorkerLocked();
        m_activeCancelFlag = cancelFlag;
        prevWorker = std::move(m_workerThread);
        m_workerThread = std::jthread([this, reqId, cursorPt, foregroundHwnd, hotkeyDisplay, cancelFlag, traceId]() {
            tools3000::core::TraceId::setCurrent(traceId);
            if (cancelFlag->load(std::memory_order_acquire)) {
                return;
            }

            // 1. 优先尝试抓取前台窗口当前选区（仅当非自身浮窗时发 Ctrl+C）
            bool fromClipboard = false;
            std::string rawText;
            const HWND selfHwnd = m_hwnd.load(std::memory_order_acquire);
            if (foregroundHwnd != selfHwnd) {
                rawText = tools3000::core::WinUtils::captureSelectedText();
            }
            if (cancelFlag->load(std::memory_order_acquire)) {
                return;
            }

            std::string cleaned = sanitizeAndTrimInput(rawText);
            // 2. 未捕获到新选区变化时无缝回退读取剪贴板纯文本
            if (cleaned.empty()) {
                const std::string clipText = tools3000::core::WinUtils::getClipboardText();
                cleaned = sanitizeAndTrimInput(clipText);
                if (!cleaned.empty()) {
                    fromClipboard = true;
                }
            }

            if (cancelFlag->load(std::memory_order_acquire)) {
                return;
            }

            if (cleaned.empty()) {
                const bool zh = tools3000::core::isUiLanguageChinese();
                const std::string keyLabel = hotkeyDisplay.empty() ? "Alt+X" : hotkeyDisplay;
                const std::wstring msg = zh
                    ? (L"请先选中或复制要翻译的文字后再按 " + tools3000::core::WinUtils::utf8ToWstring(keyLabel))
                    : (L"Select or copy text first before pressing " + tools3000::core::WinUtils::utf8ToWstring(keyLabel));
                LOG_INFO("Global translate skipped: no selected or clipboard text available");
                tools3000::core::EventBus::instance().publish(tools3000::core::ShowToastEvent{msg});
                return;
            }

            auto& svc = tools3000::core::translate::TranslationService::instance();
            const auto settings = svc.loadSettings();
            std::string sourcePref = "auto";
            std::string targetPref = "auto";
            loadQuickLanguagePrefs(sourcePref, targetPref);

            const bool lockTarget = (targetPref != "auto");
            const std::string effectiveTarget = lockTarget
                ? targetPref
                : tools3000::core::translate::TranslationService::resolveEffectiveTargetLanguage(
                      {cleaned},
                      settings.targetLang,
                      settings.secondaryTargetLang,
                      tools3000::core::UiI18n::getEffectiveLanguageTag());
            const std::string providerLabel = formatProviderDisplayLabel(settings);

            LOG_INFO("Global translate executing: reqId={}, source={}, chars={}, srcLang={}, target={}, locked={}, provider={}",
                     reqId, fromClipboard ? "clipboard" : "selection", cleaned.size(),
                     sourcePref, effectiveTarget, lockTarget, settings.provider);

            // 3. 1ms 内立即在 UI 线程弹出加载中轻量卡片
            tools3000::core::MainThreadDispatcher::instance().post(
                [this, reqId, cleaned, fromClipboard, providerLabel, sourcePref, targetPref, effectiveTarget, cursorPt]() {
                    presentLoadingOnUiThread(
                        reqId, cleaned, fromClipboard, providerLabel,
                        sourcePref, targetPref, effectiveTarget, cursorPt);
                });

            // 4. 异步调用统一 TranslationService 单一事实源（挂载长耗时阶段回调：重试/切备用引擎/分批进度）
            tools3000::core::translate::TranslationBatchRequest req;
            req.texts = {cleaned};
            req.sourceLangOverride = sourcePref;
            req.targetLangOverride = effectiveTarget;
            req.lockTargetLanguage = lockTarget;
            req.uiLanguageCode = tools3000::core::UiI18n::getEffectiveLanguageTag();
            req.onStage = [this, reqId, settings](
                              tools3000::core::translate::TranslationStage stage,
                              uint32_t current,
                              uint32_t total,
                              const std::string& detail) {
                const bool zhUi = tools3000::core::isUiLanguageChinese();
                const std::string provName =
                    tools3000::core::translate::TranslationService::formatProviderShortName(detail, zhUi);
                std::string msgUtf8;
                std::string nextProvLabel;
                if (stage == tools3000::core::translate::TranslationStage::SwitchingFallback) {
                    msgUtf8 = zhUi
                        ? ("主通道响应较慢，已自动切换至 " + provName + "...")
                        : ("Primary route slow, switching to " + provName + "...");
                    nextProvLabel = formatProviderDisplayLabel(settings, detail);
                } else if (stage == tools3000::core::translate::TranslationStage::RetryingTransient) {
                    msgUtf8 = zhUi
                        ? ("网络波动，正在自动重试 (" + provName + ")...")
                        : ("Route busy, retrying (" + provName + ")...");
                } else if (stage == tools3000::core::translate::TranslationStage::BatchProgress && total > 1) {
                    const uint32_t idx = (std::min)(current + 1u, total);
                    msgUtf8 = zhUi
                        ? ("长文本分批翻译中（第 " + std::to_string(idx) + "/" + std::to_string(total) + " 批）...")
                        : ("Translating batch " + std::to_string(idx) + "/" + std::to_string(total) + "...");
                } else {
                    return;
                }
                tools3000::core::MainThreadDispatcher::instance().post(
                    [this, reqId, msgUtf8 = std::move(msgUtf8), nextProvLabel = std::move(nextProvLabel)]() {
                        updateLoadingStageOnUiThread(reqId, msgUtf8, nextProvLabel);
                    });
            };
            const auto res = svc.translateBatch(req, cancelFlag.get());

            if (cancelFlag->load(std::memory_order_acquire) ||
                m_shuttingDown.load(std::memory_order_acquire)) {
                return;
            }

            if (res.success && !res.translations.empty() && !res.translations[0].empty()) {
                const std::string finalProvider = formatProviderDisplayLabel(settings, res.providerUsed);
                const std::string finalTarget = res.effectiveTargetLang.empty() ? effectiveTarget : res.effectiveTargetLang;
                const std::string finalDetected = res.detectedSourceLang;
                const std::string translated = res.translations[0];
                LOG_INFO("Global translate completed: reqId={}, providerUsed={}, outChars={}",
                         reqId, res.providerUsed, translated.size());
                tools3000::core::MainThreadDispatcher::instance().post(
                    [this, reqId, translated, finalDetected, finalTarget, finalProvider]() {
                        presentResultOnUiThread(reqId, translated, finalDetected, finalTarget, finalProvider);
                    });
            } else {
                const std::string errMsg = formatUserFacingError(res);
                LOG_WARN("Global translate failed: reqId={}, code={}, detail={}",
                         reqId, res.errorCode, res.errorDetail);
                tools3000::core::MainThreadDispatcher::instance().post(
                    [this, reqId, errMsg, providerLabel]() {
                        presentErrorOnUiThread(reqId, errMsg, providerLabel);
                    });
            }
        });
    }
    if (prevWorker.joinable()) {
        prevWorker.detach();
    }
}

void QuickTranslatePopupWindow::translateTextAsync(
    const std::string& textUtf8,
    bool fromClipboard,
    POINT anchorPt,
    bool bypassCache,
    const std::string& targetLangOverride,
    const std::string& sourceLangOverride) {
    if (m_shuttingDown.load(std::memory_order_acquire)) {
        return;
    }
    const std::string cleaned = sanitizeAndTrimInput(textUtf8);
    if (cleaned.empty()) {
        return;
    }

    const uint64_t reqId = m_requestSeq.fetch_add(1, std::memory_order_acq_rel) + 1;
    auto cancelFlag = std::make_shared<std::atomic<bool>>(false);
    const std::string traceId = tools3000::core::TraceId::current();

    auto& svc = tools3000::core::translate::TranslationService::instance();
    const auto settings = svc.loadSettings();

    const std::string canonSrc = tools3000::core::translate::TranslationService::canonicalizeLanguageCode(sourceLangOverride);
    const std::string sourcePref = (canonSrc.empty() || canonSrc == "auto") ? "auto" : canonSrc;
    const std::string canonDst = tools3000::core::translate::TranslationService::canonicalizeLanguageCode(targetLangOverride);
    const std::string targetPref = (canonDst.empty() || canonDst == "auto") ? "auto" : canonDst;
    const bool lockTarget = (targetPref != "auto");

    const std::string effectiveTarget = lockTarget
        ? targetPref
        : tools3000::core::translate::TranslationService::resolveEffectiveTargetLanguage(
              {cleaned},
              settings.targetLang,
              settings.secondaryTargetLang,
              tools3000::core::UiI18n::getEffectiveLanguageTag());
    const std::string providerLabel = formatProviderDisplayLabel(settings);

    tools3000::core::MainThreadDispatcher::instance().post(
        [this, reqId, cleaned, fromClipboard, providerLabel, sourcePref, targetPref, effectiveTarget, anchorPt]() {
            presentLoadingOnUiThread(
                reqId, cleaned, fromClipboard, providerLabel,
                sourcePref, targetPref, effectiveTarget, anchorPt);
        });

    std::jthread prevWorker;
    {
        std::lock_guard lock(m_workerMutex);
        cancelActiveWorkerLocked();
        m_activeCancelFlag = cancelFlag;
        prevWorker = std::move(m_workerThread);
        m_workerThread = std::jthread([this, reqId, cleaned, sourcePref, effectiveTarget, lockTarget, settings, providerLabel, bypassCache, cancelFlag, traceId]() {
            tools3000::core::TraceId::setCurrent(traceId);
            if (cancelFlag->load(std::memory_order_acquire)) return;

            tools3000::core::translate::TranslationBatchRequest req;
            req.texts = {cleaned};
            req.sourceLangOverride = sourcePref;
            req.targetLangOverride = effectiveTarget;
            req.lockTargetLanguage = lockTarget;
            req.uiLanguageCode = tools3000::core::UiI18n::getEffectiveLanguageTag();
            req.bypassCache = bypassCache;
            req.onStage = [this, reqId, settings](
                              tools3000::core::translate::TranslationStage stage,
                              uint32_t current,
                              uint32_t total,
                              const std::string& detail) {
                const bool zhUi = tools3000::core::isUiLanguageChinese();
                const std::string provName =
                    tools3000::core::translate::TranslationService::formatProviderShortName(detail, zhUi);
                std::string msgUtf8;
                std::string nextProvLabel;
                if (stage == tools3000::core::translate::TranslationStage::SwitchingFallback) {
                    msgUtf8 = zhUi
                        ? ("主通道响应较慢，已自动切换至 " + provName + "...")
                        : ("Primary route slow, switching to " + provName + "...");
                    nextProvLabel = formatProviderDisplayLabel(settings, detail);
                } else if (stage == tools3000::core::translate::TranslationStage::RetryingTransient) {
                    msgUtf8 = zhUi
                        ? ("网络波动，正在自动重试 (" + provName + ")...")
                        : ("Route busy, retrying (" + provName + ")...");
                } else if (stage == tools3000::core::translate::TranslationStage::BatchProgress && total > 1) {
                    const uint32_t idx = (std::min)(current + 1u, total);
                    msgUtf8 = zhUi
                        ? ("长文本分批翻译中（第 " + std::to_string(idx) + "/" + std::to_string(total) + " 批）...")
                        : ("Translating batch " + std::to_string(idx) + "/" + std::to_string(total) + "...");
                } else {
                    return;
                }
                tools3000::core::MainThreadDispatcher::instance().post(
                    [this, reqId, msgUtf8 = std::move(msgUtf8), nextProvLabel = std::move(nextProvLabel)]() {
                        updateLoadingStageOnUiThread(reqId, msgUtf8, nextProvLabel);
                    });
            };
            const auto res = tools3000::core::translate::TranslationService::instance().translateBatch(
                req, cancelFlag.get());

            if (cancelFlag->load(std::memory_order_acquire) ||
                m_shuttingDown.load(std::memory_order_acquire)) {
                return;
            }
            if (res.success && !res.translations.empty() && !res.translations[0].empty()) {
                const std::string finalProvider = formatProviderDisplayLabel(settings, res.providerUsed);
                const std::string finalTarget = res.effectiveTargetLang.empty() ? effectiveTarget : res.effectiveTargetLang;
                const std::string finalDetected = res.detectedSourceLang;
                const std::string translated = res.translations[0];
                tools3000::core::MainThreadDispatcher::instance().post(
                    [this, reqId, translated, finalDetected, finalTarget, finalProvider]() {
                        presentResultOnUiThread(reqId, translated, finalDetected, finalTarget, finalProvider);
                    });
            } else {
                const std::string errMsg = formatUserFacingError(res);
                tools3000::core::MainThreadDispatcher::instance().post(
                    [this, reqId, errMsg, providerLabel]() {
                        presentErrorOnUiThread(reqId, errMsg, providerLabel);
                    });
            }
        });
    }
    if (prevWorker.joinable()) {
        prevWorker.detach();
    }
}

void QuickTranslatePopupWindow::ensureWindowCreated() {
    if (HWND existing = m_hwnd.load(std::memory_order_acquire); existing && IsWindow(existing)) {
        return;
    }
    if (!m_hInstance) {
        m_hInstance = GetModuleHandleW(nullptr);
    }
    if (!m_d2dFactory || !m_dwriteFactory) {
        initialize(m_hInstance);
    }
    if (!m_helperOwnerHwnd || !IsWindow(m_helperOwnerHwnd)) {
        m_helperOwnerHwnd = tools3000::core::WinUtils::createOverlayHelperOwner(
            m_hInstance, L"Tools3000_QuickTranslateHelperOwner");
    }

    HWND hwnd = CreateWindowExW(
        WS_EX_TOPMOST | WS_EX_TOOLWINDOW,
        kPopupClassName,
        L"Tools3000 Quick Translate",
        WS_POPUP | WS_CLIPCHILDREN | WS_CLIPSIBLINGS,
        0, 0, 468, 220,
        m_helperOwnerHwnd,
        nullptr,
        m_hInstance,
        this);
    if (!hwnd) {
        LOG_ERROR("Failed to create QuickTranslatePopupWindow: error={}", GetLastError());
        return;
    }
    SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(this));
    // 严禁调用带 WS_EX_LAYERED | WS_EX_TRANSPARENT | WS_EX_NOACTIVATE 的 applyTaskbarSafeOverlayStyle：
    // 本浮窗是可交互 ID2D1HwndRenderTarget 弹窗，叠加 WS_EX_LAYERED 会导致客户区全透明成空框且鼠标点击穿透。
    SetPropW(hwnd, L"NonRudeHWND", reinterpret_cast<HANDLE>(TRUE));
    tools3000::core::WinUtils::allowWindowCapture(hwnd);
    m_hwnd.store(hwnd, std::memory_order_release);
}

bool QuickTranslatePopupWindow::createDeviceResources() {
    HWND hwnd = m_hwnd.load(std::memory_order_acquire);
    if (!hwnd || !IsWindow(hwnd) || !m_d2dFactory) {
        return false;
    }
    if (m_renderTarget) {
        return true;
    }
    RECT rc{};
    GetClientRect(hwnd, &rc);
    const D2D1_SIZE_U size = D2D1::SizeU(
        static_cast<UINT32>((std::max)(1L, rc.right - rc.left)),
        static_cast<UINT32>((std::max)(1L, rc.bottom - rc.top)));
    auto rtProps = D2D1::RenderTargetProperties();
    rtProps.dpiX = 96.0f * m_dpiScale;
    rtProps.dpiY = 96.0f * m_dpiScale;
    const HRESULT hr = m_d2dFactory->CreateHwndRenderTarget(
        rtProps,
        D2D1::HwndRenderTargetProperties(hwnd, size, D2D1_PRESENT_OPTIONS_IMMEDIATELY),
        m_renderTarget.GetAddressOf());
    if (FAILED(hr) || !m_renderTarget) {
        return false;
    }
    m_renderTarget->SetTextAntialiasMode(D2D1_TEXT_ANTIALIAS_MODE_CLEARTYPE);
    return true;
}

void QuickTranslatePopupWindow::discardDeviceResources() {
    m_renderTarget.Reset();
}

void QuickTranslatePopupWindow::rebuildTextFormats() {
    if (!m_dwriteFactory) return;
    const wchar_t* fontFamily = tools3000::core::resolveInstalledUiFontFamily(m_dwriteFactory.Get());
    const wchar_t* localeName = tools3000::core::resolveInstalledUiLocaleNameW();

    m_badgeFormat.Reset();
    m_dwriteFactory->CreateTextFormat(
        fontFamily, nullptr, DWRITE_FONT_WEIGHT_SEMI_BOLD,
        DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL,
        12.0f, localeName, m_badgeFormat.GetAddressOf());
    if (m_badgeFormat) {
        m_badgeFormat->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
        m_badgeFormat->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
    }

    m_sourceFormat.Reset();
    m_dwriteFactory->CreateTextFormat(
        fontFamily, nullptr, DWRITE_FONT_WEIGHT_MEDIUM,
        DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL,
        12.5f, localeName, m_sourceFormat.GetAddressOf());
    if (m_sourceFormat) {
        m_sourceFormat->SetWordWrapping(DWRITE_WORD_WRAPPING_WRAP);
        DWRITE_TRIMMING trimming{};
        trimming.granularity = DWRITE_TRIMMING_GRANULARITY_CHARACTER;
        Microsoft::WRL::ComPtr<IDWriteInlineObject> ellipsis;
        if (SUCCEEDED(m_dwriteFactory->CreateEllipsisTrimmingSign(m_sourceFormat.Get(), ellipsis.GetAddressOf()))) {
            m_sourceFormat->SetTrimming(&trimming, ellipsis.Get());
        }
    }

    m_bodyFormat.Reset();
    m_dwriteFactory->CreateTextFormat(
        fontFamily, nullptr, DWRITE_FONT_WEIGHT_MEDIUM,
        DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL,
        14.5f, localeName, m_bodyFormat.GetAddressOf());
    if (m_bodyFormat) {
        m_bodyFormat->SetWordWrapping(DWRITE_WORD_WRAPPING_WRAP);
        m_bodyFormat->SetLineSpacing(DWRITE_LINE_SPACING_METHOD_PROPORTIONAL, 1.5f, 1.15f);
    }

    m_btnFormat.Reset();
    m_dwriteFactory->CreateTextFormat(
        fontFamily, nullptr, DWRITE_FONT_WEIGHT_SEMI_BOLD,
        DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL,
        12.5f, localeName, m_btnFormat.GetAddressOf());
    if (m_btnFormat) {
        m_btnFormat->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
        m_btnFormat->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
        m_btnFormat->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
    }
}

float QuickTranslatePopupWindow::measureBadgeTextWidth(const std::wstring& text) const {
    if (!m_dwriteFactory || !m_badgeFormat || text.empty()) {
        return 42.0f;
    }
    Microsoft::WRL::ComPtr<IDWriteTextLayout> layout;
    if (SUCCEEDED(m_dwriteFactory->CreateTextLayout(
            text.c_str(), static_cast<UINT32>(text.size()),
            m_badgeFormat.Get(), 260.0f, 24.0f, layout.GetAddressOf())) &&
        layout) {
        DWRITE_TEXT_METRICS tm{};
        if (SUCCEEDED(layout->GetMetrics(&tm))) {
            return tm.widthIncludingTrailingWhitespace;
        }
    }
    return static_cast<float>(text.size()) * 11.0f;
}

void QuickTranslatePopupWindow::rebuildLayoutsAndResize(bool repositionAtAnchor) {
    HWND hwnd = m_hwnd.load(std::memory_order_acquire);
    if (!hwnd || !IsWindow(hwnd) || !m_dwriteFactory) {
        return;
    }

    HMONITOR mon = MonitorFromPoint(m_anchorPt, MONITOR_DEFAULTTONEAREST);
    MONITORINFO mi{};
    mi.cbSize = sizeof(mi);
    if (!mon || !GetMonitorInfoW(mon, &mi)) {
        mi.rcWork = {0, 0, GetSystemMetrics(SM_CXSCREEN), GetSystemMetrics(SM_CYSCREEN)};
    }

    UINT dpiX = 96;
    UINT dpiY = 96;
    if (mon && SUCCEEDED(GetDpiForMonitor(mon, MDT_EFFECTIVE_DPI, &dpiX, &dpiY)) && dpiX >= 96) {
        m_dpiScale = static_cast<float>(dpiX) / 96.0f;
    } else {
        m_dpiScale = tools3000::core::WinUtils::getDpiScale(hwnd);
    }

    if (!m_bodyFormat) {
        rebuildTextFormats();
    }

    const float contentWidth = kCardWidthDip - kOuterPadDip * 2.0f;

    // 1. 原文预览布局（最多 2 行 ~38 DIP 高）
    const std::wstring srcWide = tools3000::core::WinUtils::utf8ToWstring(m_sourceTextUtf8);
    m_sourceLayout.Reset();
    float srcHeight = 20.0f;
    if (m_sourceFormat) {
        if (SUCCEEDED(m_dwriteFactory->CreateTextLayout(
                srcWide.c_str(), static_cast<UINT32>(srcWide.size()),
                m_sourceFormat.Get(), contentWidth - 18.0f, 38.0f,
                m_sourceLayout.GetAddressOf())) &&
            m_sourceLayout) {
            DWRITE_TEXT_METRICS tm{};
            if (SUCCEEDED(m_sourceLayout->GetMetrics(&tm))) {
                srcHeight = std::clamp(tm.height, 18.0f, 38.0f);
            }
        }
    }
    const float sourceBoxHeight = srcHeight + 14.0f;

    // 2. 主体译文/状态布局
    const PopupState st = m_state.load(std::memory_order_acquire);
    const bool zh = tools3000::core::isUiLanguageChinese();
    std::wstring bodyWide;
    if (st == PopupState::Loading) {
        if (!m_loadingStageTextUtf8.empty()) {
            bodyWide = tools3000::core::WinUtils::utf8ToWstring(m_loadingStageTextUtf8);
        } else {
            bodyWide = (zh ? std::wstring(L"正在使用 ") : std::wstring(L"Translating via ")) +
                       tools3000::core::WinUtils::utf8ToWstring(m_providerLabelUtf8) +
                       (zh ? std::wstring(L" 翻译...") : std::wstring(L"..."));
        }
    } else if (st == PopupState::Error) {
        bodyWide = tools3000::core::WinUtils::utf8ToWstring(m_errorTextUtf8);
    } else {
        bodyWide = tools3000::core::WinUtils::utf8ToWstring(m_translatedTextUtf8);
    }

    m_bodyLayout.Reset();
    float bodyNaturalHeight = 32.0f;
    if (m_bodyFormat) {
        if (SUCCEEDED(m_dwriteFactory->CreateTextLayout(
                bodyWide.c_str(), static_cast<UINT32>(bodyWide.size()),
                m_bodyFormat.Get(), contentWidth - 8.0f, 4000.0f,
                m_bodyLayout.GetAddressOf())) &&
            m_bodyLayout) {
            DWRITE_TEXT_METRICS tm{};
            if (SUCCEEDED(m_bodyLayout->GetMetrics(&tm))) {
                bodyNaturalHeight = (std::max)(26.0f, tm.height + 6.0f);
            }
        }
    }
    m_bodyContentHeight = bodyNaturalHeight;

    const float fixedChromeHeight = kHeaderHeightDip + kSectionGapDip + sourceBoxHeight + kSectionGapDip + kFooterHeightDip + 10.0f;
    const float desiredHeightDip = std::clamp(fixedChromeHeight + bodyNaturalHeight, kMinCardHeightDip, kMaxCardHeightDip);
    const float bodyViewportH = (std::max)(44.0f, desiredHeightDip - fixedChromeHeight);

    const float bodyTopDip = kHeaderHeightDip + kSectionGapDip + sourceBoxHeight + kSectionGapDip;
    m_bodyViewportRect = D2D1::RectF(
        kOuterPadDip, bodyTopDip,
        kCardWidthDip - kOuterPadDip, bodyTopDip + bodyViewportH);
    m_maxScrollY = (std::max)(0.0f, m_bodyContentHeight - bodyViewportH);
    m_scrollY = std::clamp(m_scrollY, 0.0f, m_maxScrollY);

    // 3. 顶栏交互式语种三件套（源语种下拉 + 一键互译交换 + 目标语种下拉）精确测宽布局
    const std::string srcLabelUtf8 = formatLanguagePillLabel(m_sourceLangPref, zh, false, m_detectedSourceLang);
    const std::string dstLabelUtf8 = formatLanguagePillLabel(
        m_effectiveTargetLang, zh, m_targetLangPref == "auto");
    const std::wstring srcLabelW = tools3000::core::WinUtils::utf8ToWstring(srcLabelUtf8);
    const std::wstring dstLabelW = tools3000::core::WinUtils::utf8ToWstring(dstLabelUtf8);

    constexpr float pillH = 26.0f;
    const float pillTop = (kHeaderHeightDip - pillH) * 0.5f;
    const float srcPillW = std::clamp(measureBadgeTextWidth(srcLabelW) + 26.0f, 62.0f, 118.0f);
    const float dstPillW = std::clamp(measureBadgeTextWidth(dstLabelW) + 26.0f, 62.0f, 118.0f);
    constexpr float swapW = 26.0f;
    constexpr float langGap = 5.0f;

    m_sourceLangRect = D2D1::RectF(kOuterPadDip, pillTop, kOuterPadDip + srcPillW, pillTop + pillH);
    m_swapLangRect = D2D1::RectF(
        m_sourceLangRect.right + langGap, pillTop,
        m_sourceLangRect.right + langGap + swapW, pillTop + pillH);
    m_targetLangRect = D2D1::RectF(
        m_swapLangRect.right + langGap, pillTop,
        m_swapLangRect.right + langGap + dstPillW, pillTop + pillH);

    constexpr float iconBtnSize = 26.0f;
    const float topBtnY = (kHeaderHeightDip - iconBtnSize) * 0.5f;
    m_closeBtnRect = D2D1::RectF(
        kCardWidthDip - kOuterPadDip - iconBtnSize, topBtnY,
        kCardWidthDip - kOuterPadDip, topBtnY + iconBtnSize);
    m_pinBtnRect = D2D1::RectF(
        m_closeBtnRect.left - 6.0f - iconBtnSize, topBtnY,
        m_closeBtnRect.left - 6.0f, topBtnY + iconBtnSize);
    m_headerDragRect = D2D1::RectF(m_targetLangRect.right + 4.0f, 0.0f, m_pinBtnRect.left - 4.0f, kHeaderHeightDip);

    // 4. 底栏按钮
    const float footerTopDip = desiredHeightDip - kFooterHeightDip;
    const float btnH = 28.0f;
    const float btnY = footerTopDip + (kFooterHeightDip - btnH) * 0.5f;
    const float copyW = 104.0f;
    const float retryW = 68.0f;
    m_copyBtnRect = D2D1::RectF(
        kCardWidthDip - kOuterPadDip - copyW, btnY,
        kCardWidthDip - kOuterPadDip, btnY + btnH);
    m_retryBtnRect = D2D1::RectF(
        m_copyBtnRect.left - 8.0f - retryW, btnY,
        m_copyBtnRect.left - 8.0f, btnY + btnH);

    const int physW = static_cast<int>(std::lround(kCardWidthDip * m_dpiScale));
    const int physH = static_cast<int>(std::lround(desiredHeightDip * m_dpiScale));
    m_clientW = physW;
    m_clientH = physH;

    RECT targetRect{};
    if (repositionAtAnchor) {
        targetRect = computePopupPlacement(m_anchorPt, physW, physH, mi.rcWork);
    } else {
        RECT curRect{};
        if (GetWindowRect(hwnd, &curRect)) {
            const int x = std::clamp(static_cast<int>(curRect.left),
                                     static_cast<int>(mi.rcWork.left) + kWorkAreaMarginDip,
                                     (std::max)(static_cast<int>(mi.rcWork.left) + kWorkAreaMarginDip,
                                                static_cast<int>(mi.rcWork.right) - physW - kWorkAreaMarginDip));
            const int y = std::clamp(static_cast<int>(curRect.top),
                                     static_cast<int>(mi.rcWork.top) + kWorkAreaMarginDip,
                                     (std::max)(static_cast<int>(mi.rcWork.top) + kWorkAreaMarginDip,
                                                static_cast<int>(mi.rcWork.bottom) - physH - kWorkAreaMarginDip));
            targetRect = RECT{x, y, x + physW, y + physH};
        } else {
            targetRect = computePopupPlacement(m_anchorPt, physW, physH, mi.rcWork);
        }
    }

    SetWindowPos(
        hwnd, HWND_TOPMOST,
        targetRect.left, targetRect.top,
        physW, physH,
        SWP_NOOWNERZORDER | SWP_SHOWWINDOW);
    tools3000::core::WinUtils::applyUniversalRoundedCorners(
        hwnd, physW, physH, static_cast<int>(std::lround(kCornerRadiusDip * m_dpiScale)));

    if (m_renderTarget) {
        m_renderTarget->SetDpi(96.0f * m_dpiScale, 96.0f * m_dpiScale);
        m_renderTarget->Resize(D2D1::SizeU(
            static_cast<UINT32>((std::max)(1, physW)),
            static_cast<UINT32>((std::max)(1, physH))));
    }
}

void QuickTranslatePopupWindow::presentLoadingOnUiThread(
    uint64_t requestId,
    const std::string& sourceText,
    bool fromClipboard,
    const std::string& providerLabel,
    const std::string& sourcePref,
    const std::string& targetPref,
    const std::string& effectiveTarget,
    POINT anchorPt) {
    if (m_shuttingDown.load(std::memory_order_acquire)) return;
    if (requestId < m_activeUiRequestId) return;

    ensureWindowCreated();
    HWND hwnd = m_hwnd.load(std::memory_order_acquire);
    if (!hwnd || !IsWindow(hwnd)) return;

    const bool wasVisible = (IsWindowVisible(hwnd) != FALSE);
    m_activeUiRequestId = requestId;
    m_sourceTextUtf8 = sourceText;
    m_translatedTextUtf8.clear();
    m_loadingStageTextUtf8.clear();
    m_errorTextUtf8.clear();
    m_providerLabelUtf8 = providerLabel;
    m_sourceLangPref = sourcePref.empty() ? "auto" : sourcePref;
    m_targetLangPref = targetPref.empty() ? "auto" : targetPref;
    if (!effectiveTarget.empty() && effectiveTarget != m_effectiveTargetLang && !m_effectiveTargetLang.empty()) {
        m_lastDistinctTarget = m_effectiveTargetLang;
    }
    m_effectiveTargetLang = effectiveTarget.empty() ? "zh-CN" : effectiveTarget;
    m_detectedSourceLang = (m_sourceLangPref != "auto") ? m_sourceLangPref : std::string();
    m_fromClipboard = fromClipboard;
    m_copiedFeedbackActive = false;
    m_loadingFrame = 0;
    m_scrollY = 0.0f;
    m_anchorPt = anchorPt;
    m_state.store(PopupState::Loading, std::memory_order_release);

    // 浮窗当前已可见（例如原地切换目标语种/互译/重试）时保持当前位置不跳动
    const bool shouldReposition = !wasVisible;
    rebuildTextFormats();
    rebuildLayoutsAndResize(shouldReposition);
    SetForegroundWindow(hwnd);
    SetTimer(hwnd, kTimerLoadingAnim, 50, nullptr);
    render();
}

void QuickTranslatePopupWindow::updateLoadingStageOnUiThread(
    uint64_t requestId,
    const std::string& stageTextUtf8,
    const std::string& updatedProviderLabel) {
    if (m_shuttingDown.load(std::memory_order_acquire)) return;
    if (requestId != m_activeUiRequestId) return;
    if (m_state.load(std::memory_order_acquire) != PopupState::Loading) return;
    HWND hwnd = m_hwnd.load(std::memory_order_acquire);
    if (!hwnd || !IsWindow(hwnd) || !IsWindowVisible(hwnd)) return;

    m_loadingStageTextUtf8 = stageTextUtf8;
    if (!updatedProviderLabel.empty()) {
        m_providerLabelUtf8 = updatedProviderLabel;
    }
    rebuildLayoutsAndResize(false);
    render();
}

void QuickTranslatePopupWindow::presentResultOnUiThread(
    uint64_t requestId,
    const std::string& translatedText,
    const std::string& detectedSourceLang,
    const std::string& effectiveTargetLang,
    const std::string& providerLabel) {
    if (m_shuttingDown.load(std::memory_order_acquire)) return;
    if (requestId != m_activeUiRequestId) return;
    HWND hwnd = m_hwnd.load(std::memory_order_acquire);
    if (!hwnd || !IsWindow(hwnd) || m_state.load(std::memory_order_acquire) == PopupState::Hidden) {
        return;
    }

    KillTimer(hwnd, kTimerLoadingAnim);
    m_translatedTextUtf8 = translatedText;
    m_errorTextUtf8.clear();
    if (!detectedSourceLang.empty()) {
        m_detectedSourceLang = tools3000::core::translate::TranslationService::canonicalizeLanguageCode(detectedSourceLang);
    }
    if (!effectiveTargetLang.empty()) {
        m_effectiveTargetLang = tools3000::core::translate::TranslationService::canonicalizeLanguageCode(effectiveTargetLang);
    }
    m_providerLabelUtf8 = providerLabel;
    m_scrollY = 0.0f;
    m_state.store(PopupState::Ready, std::memory_order_release);

    rebuildLayoutsAndResize(false);
    render();
}

void QuickTranslatePopupWindow::presentErrorOnUiThread(
    uint64_t requestId,
    const std::string& errorMessage,
    const std::string& providerLabel) {
    if (m_shuttingDown.load(std::memory_order_acquire)) return;
    if (requestId != m_activeUiRequestId) return;
    HWND hwnd = m_hwnd.load(std::memory_order_acquire);
    if (!hwnd || !IsWindow(hwnd) || m_state.load(std::memory_order_acquire) == PopupState::Hidden) {
        return;
    }

    KillTimer(hwnd, kTimerLoadingAnim);
    m_errorTextUtf8 = errorMessage;
    m_providerLabelUtf8 = providerLabel;
    m_scrollY = 0.0f;
    m_state.store(PopupState::Error, std::memory_order_release);

    rebuildLayoutsAndResize(false);
    render();
}

void QuickTranslatePopupWindow::reloadTheme() {
    HWND hwnd = m_hwnd.load(std::memory_order_acquire);
    if (hwnd && IsWindow(hwnd) && IsWindowVisible(hwnd)) {
        rebuildTextFormats();
        rebuildLayoutsAndResize(false);
        render();
    }
}

void QuickTranslatePopupWindow::hide() {
    {
        std::lock_guard lock(m_workerMutex);
        cancelActiveWorkerLocked();
    }
    m_state.store(PopupState::Hidden, std::memory_order_release);
    m_pinned = false;
    m_menuOpen = false;
    m_copiedFeedbackActive = false;
    m_hoveredButton = HitButton::None;
    if (HWND hwnd = m_hwnd.load(std::memory_order_acquire); hwnd && IsWindow(hwnd)) {
        KillTimer(hwnd, kTimerLoadingAnim);
        KillTimer(hwnd, kTimerCopyReset);
        ShowWindow(hwnd, SW_HIDE);
    }
    discardDeviceResources();
    m_sourceLayout.Reset();
    m_bodyLayout.Reset();
}

bool QuickTranslatePopupWindow::isVisible() const noexcept {
    HWND hwnd = m_hwnd.load(std::memory_order_acquire);
    return hwnd && IsWindow(hwnd) && IsWindowVisible(hwnd) != FALSE;
}

QuickTranslatePopupWindow::HitButton QuickTranslatePopupWindow::hitTestButton(POINT clientPt) const noexcept {
    const float x = static_cast<float>(clientPt.x) / (std::max)(0.5f, m_dpiScale);
    const float y = static_cast<float>(clientPt.y) / (std::max)(0.5f, m_dpiScale);
    if (pointInD2DRect(m_sourceLangRect, x, y)) return HitButton::SourceLang;
    if (pointInD2DRect(m_swapLangRect, x, y)) return HitButton::SwapLang;
    if (pointInD2DRect(m_targetLangRect, x, y)) return HitButton::TargetLang;
    if (pointInD2DRect(m_closeBtnRect, x, y)) return HitButton::Close;
    if (pointInD2DRect(m_pinBtnRect, x, y)) return HitButton::Pin;
    if (m_state.load(std::memory_order_acquire) == PopupState::Ready && pointInD2DRect(m_copyBtnRect, x, y)) {
        return HitButton::Copy;
    }
    if ((m_state.load(std::memory_order_acquire) == PopupState::Ready ||
         m_state.load(std::memory_order_acquire) == PopupState::Error) &&
        pointInD2DRect(m_retryBtnRect, x, y)) {
        return HitButton::Retry;
    }
    return HitButton::None;
}

void QuickTranslatePopupWindow::copyTranslationToClipboard() {
    if (m_state.load(std::memory_order_acquire) != PopupState::Ready || m_translatedTextUtf8.empty()) {
        return;
    }
    HWND hwnd = m_hwnd.load(std::memory_order_acquire);
    if (tools3000::core::WinUtils::copyToClipboard(m_translatedTextUtf8, hwnd)) {
        m_copiedFeedbackActive = true;
        if (hwnd && IsWindow(hwnd)) {
            SetTimer(hwnd, kTimerCopyReset, 1500, nullptr);
        }
        render();
        tools3000::core::EventBus::instance().publish(tools3000::core::ShowToastEvent{
            tools3000::core::trWCstr(L"已复制译文到剪贴板", L"Translation copied to clipboard")});
    }
}

void QuickTranslatePopupWindow::triggerRetry() {
    if (m_sourceTextUtf8.empty()) return;
    translateTextAsync(
        m_sourceTextUtf8, m_fromClipboard, m_anchorPt,
        /*bypassCache=*/true, m_targetLangPref, m_sourceLangPref);
}

void QuickTranslatePopupWindow::applyLanguageSelection(
    const std::string& nextSourcePref,
    const std::string& nextTargetPref,
    bool swapTextWithResult) {
    if (m_sourceTextUtf8.empty()) return;

    const std::string safeSrc = nextSourcePref.empty() ? "auto" : nextSourcePref;
    const std::string safeDst = nextTargetPref.empty() ? "auto" : nextTargetPref;
    saveQuickLanguagePrefs(safeSrc, safeDst);

    std::string textToTranslate = m_sourceTextUtf8;
    if (swapTextWithResult &&
        m_state.load(std::memory_order_acquire) == PopupState::Ready &&
        !m_translatedTextUtf8.empty() &&
        m_translatedTextUtf8 != m_sourceTextUtf8) {
        textToTranslate = m_translatedTextUtf8;
    }

    LOG_INFO("QuickTranslate language updated: src={} -> {}, dst={} -> {}, swapText={}",
             m_sourceLangPref, safeSrc, m_targetLangPref, safeDst, swapTextWithResult);

    translateTextAsync(
        textToTranslate, m_fromClipboard, m_anchorPt,
        /*bypassCache=*/false, safeDst, safeSrc);
}

void QuickTranslatePopupWindow::swapLanguages() {
    if (m_sourceTextUtf8.empty()) return;
    const auto settings = tools3000::core::translate::TranslationService::instance().loadSettings();
    const bool explicitSourceBefore = (m_sourceLangPref != "auto");
    const LanguagePair next = computeSwappedLanguagePair(
        m_sourceLangPref,
        m_effectiveTargetLang,
        m_lastDistinctTarget,
        settings.targetLang,
        settings.secondaryTargetLang,
        tools3000::core::UiI18n::getEffectiveLanguageTag());
    applyLanguageSelection(next.sourceLang, next.targetLang, /*swapTextWithResult=*/explicitSourceBefore);
}

void QuickTranslatePopupWindow::showSourceLanguageMenu() {
    HWND hwnd = m_hwnd.load(std::memory_order_acquire);
    if (!hwnd || !IsWindow(hwnd)) return;

    HMENU menu = CreatePopupMenu();
    if (!menu) return;

    const bool zh = tools3000::core::isUiLanguageChinese();
    AppendMenuW(
        menu,
        MF_STRING | (m_sourceLangPref == "auto" ? MF_CHECKED : MF_UNCHECKED),
        kMenuIdAuto,
        zh ? L"自动检测语言" : L"Auto-detect language");
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);

    for (size_t i = 0; i < kSupportedLanguages.size(); ++i) {
        const auto& entry = kSupportedLanguages[i];
        const bool checked = (m_sourceLangPref == entry.code);
        AppendMenuW(
            menu,
            MF_STRING | (checked ? MF_CHECKED : MF_UNCHECKED),
            static_cast<UINT>(kMenuIdLangBase + i),
            zh ? entry.zhMenu : entry.enMenu);
    }

    POINT screenPt{
        static_cast<LONG>(std::lround(m_sourceLangRect.left * m_dpiScale)),
        static_cast<LONG>(std::lround((m_sourceLangRect.bottom + 3.0f) * m_dpiScale))};
    ClientToScreen(hwnd, &screenPt);

    m_menuOpen = true;
    SetForegroundWindow(hwnd);
    const UINT cmd = static_cast<UINT>(TrackPopupMenuEx(
        menu,
        TPM_LEFTALIGN | TPM_TOPALIGN | TPM_RETURNCMD | TPM_RIGHTBUTTON,
        screenPt.x, screenPt.y, hwnd, nullptr));
    DestroyMenu(menu);
    m_menuOpen = false;
    SetForegroundWindow(hwnd);

    if (cmd == kMenuIdAuto) {
        applyLanguageSelection("auto", m_targetLangPref);
    } else if (cmd >= kMenuIdLangBase && cmd < kMenuIdLangBase + kSupportedLanguages.size()) {
        const auto& chosen = kSupportedLanguages[cmd - kMenuIdLangBase];
        // 若所选源语种恰与当前锁定的目标语种相同，自动把目标切换为对偶语种避免同语种空翻
        std::string nextTarget = m_targetLangPref;
        if (nextTarget == chosen.code) {
            const auto settings = tools3000::core::translate::TranslationService::instance().loadSettings();
            const LanguagePair swapped = computeSwappedLanguagePair(
                "auto", chosen.code, m_lastDistinctTarget,
                settings.targetLang, settings.secondaryTargetLang,
                tools3000::core::UiI18n::getEffectiveLanguageTag());
            nextTarget = swapped.targetLang;
        }
        applyLanguageSelection(chosen.code, nextTarget);
    }
}

void QuickTranslatePopupWindow::showTargetLanguageMenu() {
    HWND hwnd = m_hwnd.load(std::memory_order_acquire);
    if (!hwnd || !IsWindow(hwnd)) return;

    HMENU menu = CreatePopupMenu();
    if (!menu) return;

    const bool zh = tools3000::core::isUiLanguageChinese();
    AppendMenuW(
        menu,
        MF_STRING | (m_targetLangPref == "auto" ? MF_CHECKED : MF_UNCHECKED),
        kMenuIdAuto,
        zh ? L"智能自动（按设置双向互译）" : L"Smart Auto (Bidirectional)");
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);

    for (size_t i = 0; i < kSupportedLanguages.size(); ++i) {
        const auto& entry = kSupportedLanguages[i];
        const bool checked = (m_targetLangPref != "auto" && m_targetLangPref == entry.code);
        AppendMenuW(
            menu,
            MF_STRING | (checked ? MF_CHECKED : MF_UNCHECKED),
            static_cast<UINT>(kMenuIdLangBase + i),
            zh ? entry.zhMenu : entry.enMenu);
    }

    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(
        menu,
        MF_STRING,
        kMenuIdOpenSettings,
        zh ? L"打开翻译引擎与互译设置..." : L"Open Translation Settings...");

    POINT screenPt{
        static_cast<LONG>(std::lround(m_targetLangRect.left * m_dpiScale)),
        static_cast<LONG>(std::lround((m_targetLangRect.bottom + 3.0f) * m_dpiScale))};
    ClientToScreen(hwnd, &screenPt);

    m_menuOpen = true;
    SetForegroundWindow(hwnd);
    const UINT cmd = static_cast<UINT>(TrackPopupMenuEx(
        menu,
        TPM_LEFTALIGN | TPM_TOPALIGN | TPM_RETURNCMD | TPM_RIGHTBUTTON,
        screenPt.x, screenPt.y, hwnd, nullptr));
    DestroyMenu(menu);
    m_menuOpen = false;
    SetForegroundWindow(hwnd);

    if (cmd == kMenuIdAuto) {
        applyLanguageSelection(m_sourceLangPref, "auto");
    } else if (cmd >= kMenuIdLangBase && cmd < kMenuIdLangBase + kSupportedLanguages.size()) {
        const auto& chosen = kSupportedLanguages[cmd - kMenuIdLangBase];
        // 若用户显式选了目标语种且与当前显式源语种撞车，自动把源切回自动检测避免空翻
        const std::string nextSource = (m_sourceLangPref == chosen.code) ? std::string("auto") : m_sourceLangPref;
        applyLanguageSelection(nextSource, chosen.code);
    } else if (cmd == kMenuIdOpenSettings) {
        if (!m_pinned) {
            hide();
        }
        tools3000::core::EventBus::instance().publish(tools3000::core::ActionOpenTranslateSettingsEvent{});
    }
}

void QuickTranslatePopupWindow::render() {
    HWND hwnd = m_hwnd.load(std::memory_order_acquire);
    if (!hwnd || !IsWindow(hwnd) || m_state.load(std::memory_order_acquire) == PopupState::Hidden) {
        return;
    }
    if (!createDeviceResources() || !m_renderTarget) {
        return;
    }

    const auto palette = PopupPalette::current();
    const bool zh = tools3000::core::isUiLanguageChinese();
    const float w = static_cast<float>(m_clientW) / (std::max)(0.5f, m_dpiScale);
    const float h = static_cast<float>(m_clientH) / (std::max)(0.5f, m_dpiScale);

    m_renderTarget->BeginDraw();
    m_renderTarget->Clear(palette.bg);

    Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> brush;
    m_renderTarget->CreateSolidColorBrush(palette.border, brush.GetAddressOf());
    if (!brush) {
        m_renderTarget->EndDraw();
        return;
    }

    // 1. 精致描边外框
    brush->SetColor(palette.border);
    m_renderTarget->DrawRoundedRectangle(
        D2D1::RoundedRect(D2D1::RectF(0.5f, 0.5f, w - 0.5f, h - 0.5f), kCornerRadiusDip, kCornerRadiusDip),
        brush.Get(), 1.0f);

    // 2. 顶栏交互式语言三件套：[源语种 ▾] [⇄] [目标语种 ▾]
    auto drawLangPill = [&](const D2D1_RECT_F& rc, const std::wstring& label, bool accentStyle, bool hovered) {
        if (hovered) {
            brush->SetColor(palette.accent);
        } else if (accentStyle) {
            brush->SetColor(palette.badgeBg);
        } else {
            brush->SetColor(palette.pillBg);
        }
        m_renderTarget->FillRoundedRectangle(D2D1::RoundedRect(rc, 6.0f, 6.0f), brush.Get());

        brush->SetColor(hovered ? palette.accent : (accentStyle ? palette.badgeBorder : palette.pillBorder));
        m_renderTarget->DrawRoundedRectangle(D2D1::RoundedRect(rc, 6.0f, 6.0f), brush.Get(), 1.0f);

        const D2D1_COLOR_F fg = hovered
            ? palette.accentText
            : (accentStyle ? palette.badgeText : palette.titleText);
        if (m_badgeFormat) {
            const D2D1_RECT_F textRc = D2D1::RectF(rc.left + 8.0f, rc.top, rc.right - 16.0f, rc.bottom);
            m_badgeFormat->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
            brush->SetColor(fg);
            m_renderTarget->DrawTextW(
                label.c_str(), static_cast<UINT32>(label.size()),
                m_badgeFormat.Get(), textRc, brush.Get(), D2D1_DRAW_TEXT_OPTIONS_CLIP);
        }

        // 矢量向下展开小三角 (chevron)
        const float cx = rc.right - 9.5f;
        const float cy = (rc.top + rc.bottom) * 0.5f + 0.5f;
        brush->SetColor(fg);
        m_renderTarget->DrawLine(D2D1::Point2F(cx - 3.0f, cy - 1.8f), D2D1::Point2F(cx, cy + 1.5f), brush.Get(), 1.4f);
        m_renderTarget->DrawLine(D2D1::Point2F(cx, cy + 1.5f), D2D1::Point2F(cx + 3.0f, cy - 1.8f), brush.Get(), 1.4f);
    };

    const std::wstring srcLabelW = tools3000::core::WinUtils::utf8ToWstring(
        formatLanguagePillLabel(m_sourceLangPref, zh, false, m_detectedSourceLang));
    const std::wstring dstLabelW = tools3000::core::WinUtils::utf8ToWstring(
        formatLanguagePillLabel(m_effectiveTargetLang, zh, m_targetLangPref == "auto"));

    drawLangPill(m_sourceLangRect, srcLabelW, m_sourceLangPref != "auto", m_hoveredButton == HitButton::SourceLang);

    // 2a. 中间一键互译交换按钮（矢量双向水平箭头）
    {
        const bool hovered = (m_hoveredButton == HitButton::SwapLang);
        if (hovered) {
            brush->SetColor(palette.btnHoverBg);
            m_renderTarget->FillRoundedRectangle(D2D1::RoundedRect(m_swapLangRect, 6.0f, 6.0f), brush.Get());
        }
        const float cx = (m_swapLangRect.left + m_swapLangRect.right) * 0.5f;
        const float cy = (m_swapLangRect.top + m_swapLangRect.bottom) * 0.5f;
        brush->SetColor(hovered ? palette.accent : palette.mutedText);
        // 上箭头向右 (->)
        m_renderTarget->DrawLine(D2D1::Point2F(cx - 5.5f, cy - 2.6f), D2D1::Point2F(cx + 5.5f, cy - 2.6f), brush.Get(), 1.4f);
        m_renderTarget->DrawLine(D2D1::Point2F(cx + 2.8f, cy - 5.0f), D2D1::Point2F(cx + 5.5f, cy - 2.6f), brush.Get(), 1.4f);
        m_renderTarget->DrawLine(D2D1::Point2F(cx + 2.8f, cy - 0.2f), D2D1::Point2F(cx + 5.5f, cy - 2.6f), brush.Get(), 1.4f);
        // 下箭头向左 (<-)
        m_renderTarget->DrawLine(D2D1::Point2F(cx + 5.5f, cy + 2.6f), D2D1::Point2F(cx - 5.5f, cy + 2.6f), brush.Get(), 1.4f);
        m_renderTarget->DrawLine(D2D1::Point2F(cx - 2.8f, cy + 0.2f), D2D1::Point2F(cx - 5.5f, cy + 2.6f), brush.Get(), 1.4f);
        m_renderTarget->DrawLine(D2D1::Point2F(cx - 2.8f, cy + 5.0f), D2D1::Point2F(cx - 5.5f, cy + 2.6f), brush.Get(), 1.4f);
    }

    drawLangPill(m_targetLangRect, dstLabelW, true, m_hoveredButton == HitButton::TargetLang);

    // 2b. 顶栏中部：引擎短名 + 来源标签（兼拖拽区）
    const std::wstring sourceTag = m_fromClipboard
        ? tools3000::core::trW(L"剪贴板", L"Clipboard")
        : tools3000::core::trW(L"划词", L"Selection");
    const std::wstring providerWide = tools3000::core::WinUtils::utf8ToWstring(m_providerLabelUtf8) +
                                      L" · " + sourceTag;
    const D2D1_RECT_F providerRc = D2D1::RectF(
        m_targetLangRect.right + 10.0f, 10.0f,
        (std::max)(m_targetLangRect.right + 10.0f, m_pinBtnRect.left - 8.0f), 34.0f);
    if (m_badgeFormat && providerRc.right > providerRc.left + 20.0f) {
        m_badgeFormat->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
        brush->SetColor(palette.mutedText);
        m_renderTarget->DrawTextW(
            providerWide.c_str(), static_cast<UINT32>(providerWide.size()),
            m_badgeFormat.Get(), providerRc, brush.Get(), D2D1_DRAW_TEXT_OPTIONS_CLIP);
    }

    // 2c. 顶栏纯矢量按钮：图钉固定 + 关闭 X
    auto drawIconButtonBg = [&](const D2D1_RECT_F& rc, bool active, bool hovered) {
        if (active) {
            brush->SetColor(palette.badgeBg);
            m_renderTarget->FillRoundedRectangle(D2D1::RoundedRect(rc, 6.0f, 6.0f), brush.Get());
        } else if (hovered) {
            brush->SetColor(palette.btnHoverBg);
            m_renderTarget->FillRoundedRectangle(D2D1::RoundedRect(rc, 6.0f, 6.0f), brush.Get());
        }
    };

    drawIconButtonBg(m_pinBtnRect, m_pinned, m_hoveredButton == HitButton::Pin);
    {
        const float cx = (m_pinBtnRect.left + m_pinBtnRect.right) * 0.5f;
        const float cy = (m_pinBtnRect.top + m_pinBtnRect.bottom) * 0.5f;
        brush->SetColor(m_pinned ? palette.accent : palette.mutedText);
        m_renderTarget->DrawLine(D2D1::Point2F(cx - 4.0f, cy - 4.5f), D2D1::Point2F(cx + 4.0f, cy - 4.5f), brush.Get(), 1.5f);
        m_renderTarget->DrawLine(D2D1::Point2F(cx - 3.0f, cy - 4.5f), D2D1::Point2F(cx - 3.0f, cy + 0.5f), brush.Get(), 1.4f);
        m_renderTarget->DrawLine(D2D1::Point2F(cx + 3.0f, cy - 4.5f), D2D1::Point2F(cx + 3.0f, cy + 0.5f), brush.Get(), 1.4f);
        m_renderTarget->DrawLine(D2D1::Point2F(cx - 5.0f, cy + 0.5f), D2D1::Point2F(cx + 5.0f, cy + 0.5f), brush.Get(), 1.5f);
        m_renderTarget->DrawLine(D2D1::Point2F(cx, cy + 0.5f), D2D1::Point2F(cx, cy + 5.5f), brush.Get(), 1.6f);
    }

    drawIconButtonBg(m_closeBtnRect, false, m_hoveredButton == HitButton::Close);
    {
        const float cx = (m_closeBtnRect.left + m_closeBtnRect.right) * 0.5f;
        const float cy = (m_closeBtnRect.top + m_closeBtnRect.bottom) * 0.5f;
        brush->SetColor(m_hoveredButton == HitButton::Close ? palette.titleText : palette.mutedText);
        m_renderTarget->DrawLine(D2D1::Point2F(cx - 4.5f, cy - 4.5f), D2D1::Point2F(cx + 4.5f, cy + 4.5f), brush.Get(), 1.6f);
        m_renderTarget->DrawLine(D2D1::Point2F(cx + 4.5f, cy - 4.5f), D2D1::Point2F(cx - 4.5f, cy + 4.5f), brush.Get(), 1.6f);
    }

    // 顶栏底部分割线
    brush->SetColor(palette.divider);
    m_renderTarget->DrawLine(
        D2D1::Point2F(kOuterPadDip, kHeaderHeightDip),
        D2D1::Point2F(w - kOuterPadDip, kHeaderHeightDip),
        brush.Get(), 1.0f);

    // 3. 原文柔和折叠预览卡片
    const float sourceBoxTop = kHeaderHeightDip + kSectionGapDip;
    const float sourceBoxBottom = m_bodyViewportRect.top - kSectionGapDip;
    const D2D1_RECT_F sourceBoxRc = D2D1::RectF(kOuterPadDip, sourceBoxTop, w - kOuterPadDip, sourceBoxBottom);
    brush->SetColor(palette.sourceBoxBg);
    m_renderTarget->FillRoundedRectangle(D2D1::RoundedRect(sourceBoxRc, 8.0f, 8.0f), brush.Get());
    if (m_sourceLayout) {
        brush->SetColor(palette.sourceText);
        m_renderTarget->PushAxisAlignedClip(sourceBoxRc, D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);
        m_renderTarget->DrawTextLayout(
            D2D1::Point2F(sourceBoxRc.left + 9.0f, sourceBoxRc.top + 7.0f),
            m_sourceLayout.Get(), brush.Get(), D2D1_DRAW_TEXT_OPTIONS_CLIP);
        m_renderTarget->PopAxisAlignedClip();
    }

    // 4. 译文 / 加载态 / 错误态主区域（带裁剪滚动、纯矢量加载旋转圆弧与细圆角滚动条）
    const PopupState st = m_state.load(std::memory_order_acquire);
    if (m_bodyLayout) {
        m_renderTarget->PushAxisAlignedClip(m_bodyViewportRect, D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);
        float bodyTextLeft = m_bodyViewportRect.left + 2.0f;
        if (st == PopupState::Loading && m_d2dFactory) {
            const float sr = 6.5f;
            const float scx = m_bodyViewportRect.left + 10.0f;
            const float scy = m_bodyViewportRect.top + 11.5f;
            const float sw = 1.8f;
            brush->SetColor(palette.scrollTrack);
            m_renderTarget->DrawEllipse(D2D1::Ellipse(D2D1::Point2F(scx, scy), sr, sr), brush.Get(), sw);
            constexpr float kPi = 3.14159265f;
            const float a0 = static_cast<float>(m_loadingFrame) * 0.34f;
            const float a1 = a0 + 1.45f * kPi;
            Microsoft::WRL::ComPtr<ID2D1PathGeometry> arcGeo;
            if (SUCCEEDED(m_d2dFactory->CreatePathGeometry(arcGeo.GetAddressOf())) && arcGeo) {
                Microsoft::WRL::ComPtr<ID2D1GeometrySink> sink;
                if (SUCCEEDED(arcGeo->Open(sink.GetAddressOf())) && sink) {
                    sink->BeginFigure(D2D1::Point2F(scx + sr * std::cos(a0), scy + sr * std::sin(a0)),
                                      D2D1_FIGURE_BEGIN_HOLLOW);
                    sink->AddArc(D2D1::ArcSegment(
                        D2D1::Point2F(scx + sr * std::cos(a1), scy + sr * std::sin(a1)),
                        D2D1::SizeF(sr, sr), 0.0f,
                        D2D1_SWEEP_DIRECTION_CLOCKWISE, D2D1_ARC_SIZE_LARGE));
                    sink->EndFigure(D2D1_FIGURE_END_OPEN);
                    if (SUCCEEDED(sink->Close())) {
                        brush->SetColor(palette.accent);
                        m_renderTarget->DrawGeometry(arcGeo.Get(), brush.Get(), sw);
                    }
                }
            }
            bodyTextLeft = m_bodyViewportRect.left + 24.0f;
        }
        if (st == PopupState::Error) {
            brush->SetColor(palette.errorText);
        } else if (st == PopupState::Loading) {
            brush->SetColor(palette.mutedText);
        } else {
            brush->SetColor(palette.bodyText);
        }
        m_renderTarget->DrawTextLayout(
            D2D1::Point2F(bodyTextLeft, m_bodyViewportRect.top - m_scrollY),
            m_bodyLayout.Get(), brush.Get(), D2D1_DRAW_TEXT_OPTIONS_CLIP);
        m_renderTarget->PopAxisAlignedClip();
    }

    if (m_maxScrollY > 1.0f) {
        const float trackH = m_bodyViewportRect.bottom - m_bodyViewportRect.top;
        const float thumbH = (std::max)(24.0f, trackH * (trackH / (std::max)(trackH, m_bodyContentHeight)));
        const float thumbY = m_bodyViewportRect.top + (m_scrollY / m_maxScrollY) * (trackH - thumbH);
        const D2D1_RECT_F trackRc = D2D1::RectF(
            m_bodyViewportRect.right - 4.0f, m_bodyViewportRect.top,
            m_bodyViewportRect.right, m_bodyViewportRect.bottom);
        const D2D1_RECT_F thumbRc = D2D1::RectF(
            m_bodyViewportRect.right - 4.0f, thumbY,
            m_bodyViewportRect.right, thumbY + thumbH);
        brush->SetColor(palette.scrollTrack);
        m_renderTarget->FillRoundedRectangle(D2D1::RoundedRect(trackRc, 2.0f, 2.0f), brush.Get());
        brush->SetColor(palette.scrollThumb);
        m_renderTarget->FillRoundedRectangle(D2D1::RoundedRect(thumbRc, 2.0f, 2.0f), brush.Get());
    }

    // 5. 底栏分割线 + 快捷提示 + 重试 & 复制按钮
    const float footerTop = h - kFooterHeightDip;
    brush->SetColor(palette.divider);
    m_renderTarget->DrawLine(
        D2D1::Point2F(kOuterPadDip, footerTop),
        D2D1::Point2F(w - kOuterPadDip, footerTop),
        brush.Get(), 1.0f);

    if (m_badgeFormat) {
        const std::wstring hint = zh
            ? L"点语种选互译 · Ctrl+C 复制 · Esc"
            : L"Pick langs / Swap · Ctrl+C · Esc";
        m_badgeFormat->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
        brush->SetColor(palette.mutedText);
        m_renderTarget->DrawTextW(
            hint.c_str(), static_cast<UINT32>(hint.size()),
            m_badgeFormat.Get(),
            D2D1::RectF(kOuterPadDip, footerTop, m_retryBtnRect.left - 8.0f, h),
            brush.Get(), D2D1_DRAW_TEXT_OPTIONS_CLIP);
    }

    if (st == PopupState::Ready || st == PopupState::Error) {
        brush->SetColor(m_hoveredButton == HitButton::Retry ? palette.btnHoverBg : palette.btnBg);
        m_renderTarget->FillRoundedRectangle(D2D1::RoundedRect(m_retryBtnRect, 6.0f, 6.0f), brush.Get());
        if (m_btnFormat) {
            const std::wstring retryLabel = zh ? L"重试" : L"Retry";
            brush->SetColor(palette.titleText);
            m_renderTarget->DrawTextW(
                retryLabel.c_str(), static_cast<UINT32>(retryLabel.size()),
                m_btnFormat.Get(), m_retryBtnRect, brush.Get());
        }
    }

    if (st == PopupState::Ready) {
        brush->SetColor(m_hoveredButton == HitButton::Copy ? palette.accentHover : palette.accent);
        m_renderTarget->FillRoundedRectangle(D2D1::RoundedRect(m_copyBtnRect, 6.0f, 6.0f), brush.Get());
        if (m_btnFormat) {
            const std::wstring copyLabel = m_copiedFeedbackActive
                ? (zh ? L"已复制" : L"Copied")
                : (zh ? L"复制译文" : L"Copy Result");
            brush->SetColor(palette.accentText);
            m_renderTarget->DrawTextW(
                copyLabel.c_str(), static_cast<UINT32>(copyLabel.size()),
                m_btnFormat.Get(), m_copyBtnRect, brush.Get());
        }
    }

    const HRESULT hr = m_renderTarget->EndDraw();
    if (hr == D2DERR_RECREATE_TARGET) {
        discardDeviceResources();
    }
}

LRESULT CALLBACK QuickTranslatePopupWindow::windowProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    QuickTranslatePopupWindow* self = nullptr;
    if (msg == WM_NCCREATE) {
        const auto* cs = reinterpret_cast<CREATESTRUCTW*>(lParam);
        self = static_cast<QuickTranslatePopupWindow*>(cs->lpCreateParams);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
    } else {
        self = reinterpret_cast<QuickTranslatePopupWindow*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    }
    if (self) {
        return self->handleMessage(hwnd, msg, wParam, lParam);
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

LRESULT QuickTranslatePopupWindow::handleMessage(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
        case WM_ERASEBKGND:
            return 1;
        case WM_PAINT: {
            PAINTSTRUCT ps{};
            BeginPaint(hwnd, &ps);
            render();
            EndPaint(hwnd, &ps);
            return 0;
        }
        case WM_TIMER: {
            if (wParam == kTimerLoadingAnim) {
                if (m_state.load(std::memory_order_acquire) == PopupState::Loading) {
                    ++m_loadingFrame;
                    render();
                } else {
                    KillTimer(hwnd, kTimerLoadingAnim);
                }
                return 0;
            }
            if (wParam == kTimerCopyReset) {
                KillTimer(hwnd, kTimerCopyReset);
                m_copiedFeedbackActive = false;
                render();
                return 0;
            }
            break;
        }
        case WM_ACTIVATE: {
            if (LOWORD(wParam) == WA_INACTIVE && !m_pinned && !m_menuOpen) {
                hide();
            }
            return 0;
        }
        case WM_KEYDOWN: {
            if (wParam == VK_ESCAPE) {
                hide();
                return 0;
            }
            if (wParam == 'C' && ((GetKeyState(VK_CONTROL) & 0x8000) != 0)) {
                copyTranslationToClipboard();
                return 0;
            }
            break;
        }
        case WM_MOUSEWHEEL: {
            if (m_maxScrollY > 0.0f) {
                const short delta = GET_WHEEL_DELTA_WPARAM(wParam);
                m_scrollY = std::clamp(m_scrollY - static_cast<float>(delta) * 0.45f, 0.0f, m_maxScrollY);
                render();
            }
            return 0;
        }
        case WM_MOUSEMOVE: {
            if (!m_trackingMouseLeave) {
                TRACKMOUSEEVENT tme{sizeof(tme), TME_LEAVE, hwnd, 0};
                TrackMouseEvent(&tme);
                m_trackingMouseLeave = true;
            }
            const POINT pt{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
            const HitButton next = hitTestButton(pt);
            if (next != m_hoveredButton) {
                m_hoveredButton = next;
                render();
            }
            return 0;
        }
        case WM_MOUSELEAVE: {
            m_trackingMouseLeave = false;
            if (m_hoveredButton != HitButton::None) {
                m_hoveredButton = HitButton::None;
                render();
            }
            return 0;
        }
        case WM_SETCURSOR: {
            if (LOWORD(lParam) == HTCLIENT) {
                POINT pt{};
                if (GetCursorPos(&pt) && ScreenToClient(hwnd, &pt)) {
                    const HitButton btn = hitTestButton(pt);
                    SetCursor(LoadCursor(nullptr, btn != HitButton::None ? IDC_HAND : IDC_ARROW));
                    return TRUE;
                }
            }
            break;
        }
        case WM_LBUTTONDOWN: {
            const POINT pt{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
            const HitButton btn = hitTestButton(pt);
            if (btn == HitButton::SourceLang) {
                showSourceLanguageMenu();
                return 0;
            }
            if (btn == HitButton::SwapLang) {
                swapLanguages();
                return 0;
            }
            if (btn == HitButton::TargetLang) {
                showTargetLanguageMenu();
                return 0;
            }
            if (btn == HitButton::Close) {
                hide();
                return 0;
            }
            if (btn == HitButton::Pin) {
                m_pinned = !m_pinned;
                render();
                return 0;
            }
            if (btn == HitButton::Copy) {
                copyTranslationToClipboard();
                return 0;
            }
            if (btn == HitButton::Retry) {
                triggerRetry();
                return 0;
            }
            const float xDip = static_cast<float>(pt.x) / (std::max)(0.5f, m_dpiScale);
            const float yDip = static_cast<float>(pt.y) / (std::max)(0.5f, m_dpiScale);
            if (pointInD2DRect(m_headerDragRect, xDip, yDip)) {
                ReleaseCapture();
                SendMessageW(hwnd, WM_NCLBUTTONDOWN, HTCAPTION, 0);
                return 0;
            }
            return 0;
        }
        case WM_DPICHANGED: {
            const UINT newDpi = HIWORD(wParam);
            if (newDpi >= 96) {
                m_dpiScale = static_cast<float>(newDpi) / 96.0f;
            }
            rebuildTextFormats();
            rebuildLayoutsAndResize(false);
            render();
            return 0;
        }
        default:
            break;
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

}  // namespace tools3000::ui
