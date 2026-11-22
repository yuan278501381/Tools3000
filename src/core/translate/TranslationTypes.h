#pragma once
// ─────────────────────────────────────────────────────────────────────────────
// TranslationTypes.h — 跨模块通用翻译引擎契约模型与常量单一事实源 (SSOT)
// ─────────────────────────────────────────────────────────────────────────────

#ifndef TOOLS3000_CORE_TRANSLATE_TRANSLATIONTYPES_H
#define TOOLS3000_CORE_TRANSLATE_TRANSLATIONTYPES_H

#include <cstdint>
#include <string>
#include <vector>

namespace tools3000::core::translate {

namespace TranslationConstants {
    inline constexpr const char* kProviderAutoFree = "auto_free";
    inline constexpr const char* kProviderOpenAiCompatible = "openai_compatible";
    inline constexpr const char* kProviderDeepL = "deepl";

    inline constexpr const char* kFreePrimaryEdge = "edge";
    inline constexpr const char* kFreePrimaryGoogle = "google";

    inline constexpr const char* kTargetLangAuto = "auto";
    inline constexpr const char* kDefaultSecondaryTargetLang = "en";

    inline constexpr const char* kDefaultOpenAiBaseUrl = "https://api.openai.com/v1";
    inline constexpr const char* kDefaultOpenAiModel = "gpt-4o-mini";

    inline constexpr int kDefaultTimeoutMs = 15000;
    inline constexpr int kMinTimeoutMs = 3000;
    inline constexpr int kMaxTimeoutMs = 60000;

    inline constexpr size_t kMaxHttpResponseBytes = 4u * 1024u * 1024u;
    inline constexpr size_t kMaxBatchCharacters = 18000u;
    inline constexpr int kEdgeTokenTtlSeconds = 480; // 8 分钟，避免 10 分钟过期边界撞车
    inline constexpr float kAutoCjkThresholdRatio = 0.28f;
} // namespace TranslationConstants

/// 翻译提供方类型
enum class TranslationProviderKind : int {
    AutoFree = 0,         // 免 Key 免费双源（Microsoft Edge 主通道 + Google Translate 备选）
    OpenAiCompatible = 1, // BYOK OpenAI 兼容协议（OpenAI / DeepSeek / Gemini / 中转站 / 本地 Ollama）
    DeepL = 2,            // BYOK DeepL API（自动按 :fx 后缀路由 Free/Pro，支持自定义端点）
};

/// 持久化翻译设置快照（与 ConfigManager /translate/* 单一事实源对应）
struct TranslationSettings {
    std::string provider = TranslationConstants::kProviderAutoFree;
    std::string freePrimary = TranslationConstants::kFreePrimaryEdge;
    std::string targetLang = TranslationConstants::kTargetLangAuto;
    std::string secondaryTargetLang = TranslationConstants::kDefaultSecondaryTargetLang;

    // BYOK: OpenAI Compatible / Ollama
    std::string openaiBaseUrl = TranslationConstants::kDefaultOpenAiBaseUrl;
    std::string openaiApiKey; // 内存态明文；持久化自动经 Windows DPAPI 加密
    std::string openaiModel = TranslationConstants::kDefaultOpenAiModel;

    // BYOK: DeepL
    std::string deeplBaseUrl;
    std::string deeplApiKey; // 内存态明文；持久化自动经 Windows DPAPI 加密

    int timeoutMs = TranslationConstants::kDefaultTimeoutMs;
};

/// 批量翻译请求载荷
struct TranslationBatchRequest {
    std::vector<std::string> texts;
    std::string targetLangOverride; // 非空时覆盖配置中的 targetLang
    std::string uiLanguageCode;     // 当前界面语种（如 "zh-CN" / "en" / "ja"），用于 auto 智能判定
};

/// 批量翻译结果载荷
struct TranslationBatchResult {
    bool success = false;
    std::vector<std::string> translations;
    std::string providerUsed;        // "edge_free" | "google_free" | "openai_compatible" | "deepl"
    std::string detectedSourceLang;  // 如 "en" / "zh-Hans" / "ja"
    std::string effectiveTargetLang; // 实际翻译目标语种代码
    uint32_t elapsedMs = 0;
    std::string errorCode;           // "empty_input" | "cancelled" | "missing_api_key" | "invalid_endpoint" | "network" | "http_error" | "rate_limited" | "invalid_response"
    std::string errorDetail;
};

} // namespace tools3000::core::translate

#endif // TOOLS3000_CORE_TRANSLATE_TRANSLATIONTYPES_H
