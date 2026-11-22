#pragma once
// ─────────────────────────────────────────────────────────────────────────────
// TranslationService.h — 跨模块通用多引擎翻译服务 (Tools3000Core 导出)
//
// 职责:
//   1. 通过多态策略分发统一管理免 Key 免费双源 (Edge 主 + Google 备)
//      与 BYOK (OpenAI 兼容 / DeepSeek / 本地 Ollama / DeepL)；
//   2. 自动继承 Windows 系统代理、DPAPI 加密保存 API Key、严禁日志泄露凭据；
//   3. 严格按需联网（仅用户显式触发翻译或连通性测试时发请求，零后台静默流量）。
// ─────────────────────────────────────────────────────────────────────────────

#ifndef TOOLS3000_CORE_TRANSLATE_TRANSLATIONSERVICE_H
#define TOOLS3000_CORE_TRANSLATE_TRANSLATIONSERVICE_H

#include "core/utils/Export.h"
#include "core/translate/TranslationTypes.h"

#include <atomic>
#include <mutex>
#include <string>
#include <string_view>
#include <vector>

namespace tools3000::core::translate {

class TOOLS3000CORE_API TranslationService final {
public:
    static TranslationService& instance();

    /// 读取持久化翻译设置（自动解密 DPAPI 密钥）
    TranslationSettings loadSettings() const;

    /// 保存翻译设置（自动使用 DPAPI 加密密钥，严禁明文落盘）
    bool saveSettings(const TranslationSettings& settings);

    /// 使用当前配置执行批量翻译（线程安全；cancelFlag 置 true 时尽快中止）
    TranslationBatchResult translateBatch(
        const TranslationBatchRequest& request,
        const std::atomic<bool>* cancelFlag = nullptr);

    /// 使用指定配置执行批量翻译（供连通性测试与单元测试注入使用）
    TranslationBatchResult translateBatchWithSettings(
        const TranslationBatchRequest& request,
        const TranslationSettings& settings,
        const std::atomic<bool>* cancelFlag = nullptr);

    /// 清除内存中的临时令牌缓存（插件卸载或切换网络环境时调用）
    void clearTokenCache();

    // ── 纯静态工具与解析函数（单一事实源，100% 离线单元测试覆盖） ──────────────

    static TranslationProviderKind parseProviderKind(std::string_view provider) noexcept;
    static std::string canonicalizeLanguageCode(std::string_view raw);

    /// 智能双向目标语种判定：targetLang == "auto" 时按样本 CJK 占比自动切换主/副目标语种
    static std::string resolveEffectiveTargetLanguage(
        const std::vector<std::string>& texts,
        std::string_view configuredTargetLang,
        std::string_view secondaryTargetLang,
        std::string_view uiLanguageCode);

    /// 规范化 OpenAI 兼容接口地址（自动补全 /chat/completions，允许本地 localhost HTTP）
    static bool normalizeOpenAiChatUrl(std::string_view baseUrl, std::string& outFullUrl);

    /// 判断 URL 是否指向本机环回地址（localhost / 127.0.0.1 / [::1]），本地 Ollama 免 Key 放行
    static bool isLoopbackHttpUrl(std::string_view url) noexcept;

    /// 规范化 DeepL 端点地址（空值时自动按 :fx 后缀路由免费/专业端点）
    static bool normalizeDeepLUrl(std::string_view customBaseUrl,
                                  std::string_view apiKey,
                                  std::string& outFullUrl);

    /// 将目标语言代码映射到各引擎协议规范代码
    static std::string toEdgeLangCode(std::string_view canonicalLang);
    static std::string toGoogleLangCode(std::string_view canonicalLang);
    static std::string toDeepLLangCode(std::string_view canonicalLang);
    static std::string toHumanLanguageName(std::string_view canonicalLang);

    /// 协议响应解析器（纯函数，支持单元测试精确验证各类边界响应）
    static bool parseEdgeTranslateResponse(
        std::string_view responseBody,
        size_t expectedCount,
        std::vector<std::string>& outTranslations,
        std::string& outDetectedLang);

    static bool parseGoogleTranslateResponse(
        std::string_view responseBody,
        std::string& outMergedText,
        std::string& outDetectedLang);

    static bool parseOpenAiBatchResponse(
        std::string_view responseBody,
        size_t expectedCount,
        std::vector<std::string>& outTranslations);

    static bool parseDeepLTranslateResponse(
        std::string_view responseBody,
        size_t expectedCount,
        std::vector<std::string>& outTranslations,
        std::string& outDetectedLang);

    /// 从微软 Bing/Edge 翻译页 HTML 提取按需会话凭据（纯函数，支持离线单测）
    static bool parseBingTranslatorCredentials(
        std::string_view htmlBody,
        std::string& outIg,
        std::string& outIid,
        std::string& outKey,
        std::string& outToken);

    /// Windows DPAPI 密钥持久化加密/解密与前端遮罩辅助
    static std::string protectSecretForStorage(std::string_view plainSecret);
    static std::string unprotectSecretFromStorage(std::string_view storedSecret);
    static std::string maskApiKeyForUi(std::string_view plainSecret);

private:
    TranslationService() = default;
    ~TranslationService() = default;
    TranslationService(const TranslationService&) = delete;
    TranslationService& operator=(const TranslationService&) = delete;

    struct BingSessionCreds {
        std::string host;
        std::string ig;
        std::string iid;
        std::string key;
        std::string token;
        int64_t expireUnix = 0;
        uint32_t reqSeq = 0;
    };

    std::mutex m_edgeTokenMutex;
    BingSessionCreds m_cachedBingCreds;
    std::atomic<int64_t> m_edgeCooldownUntilUnix{0};

    bool acquireBingCredentials(int timeoutMs,
                                const std::atomic<bool>* cancelFlag,
                                BingSessionCreds& outCreds,
                                std::string& outErrorCode,
                                std::string& outErrorDetail);
};

} // namespace tools3000::core::translate

#endif // TOOLS3000_CORE_TRANSLATE_TRANSLATIONSERVICE_H
