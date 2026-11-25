// ─────────────────────────────────────────────────────────────────────────────
// TranslationService.cpp — 跨模块通用多引擎翻译服务实现
// ─────────────────────────────────────────────────────────────────────────────

#include "core/translate/TranslationService.h"

#include "core/config/ConfigManager.h"
#include "core/logger/Logger.h"
#include "core/utils/WinUtils.h"

#include <windows.h>
#include <winhttp.h>
#include <wincrypt.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <chrono>
#include <cmath>
#include <limits>
#include <list>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

namespace tools3000::core::translate {

namespace {

using json = nlohmann::json;

constexpr const char* kDpapiPrefix = "dpapi:";
constexpr const char* kGoogleBatchSentinel = "\n[[T3K_SEG_91827]]\n";
constexpr const char* kGoogleBatchSentinelCore = "T3K_SEG_91827";
constexpr size_t kMaxConcurrentTranslateBatches = 4;
constexpr size_t kMaxSessionCacheEntries = 512;
constexpr int kTransientRetryDelayMs = 600;
constexpr int kTransientRetrySliceMs = 50;
constexpr int kMaxBinarySplitDepth = 4;

// 渠道隔离 User-Agent 常量：
// - 免费网页通道（Edge/Google）使用标准浏览器 UA；
// - Bilibili Index-Translate 官方公开接口强制使用官方客户端 UA，避免被 CDN WAF 对无 Cookie 浏览器伪装 POST 返回 412；
// - 通用 OpenAI 兼容 / DeepL / 本机 Ollama API 使用诚实原生客户端 UA，杜绝第三方 WAF 误拦。
constexpr const wchar_t* kBrowserUserAgent =
    L"Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/131.0.0.0 Safari/537.36 Edg/131.0.0.0";
constexpr const wchar_t* kIndexTranslateUserAgent = L"Index-Translate-Client/1.0";
constexpr const wchar_t* kApiClientUserAgent = L"Tools3000/1.4";

bool sleepCancelAwareMs(
    int totalMs,
    const std::atomic<bool>* cancelFlag,
    const std::atomic<bool>& localAbort) noexcept {
    int elapsed = 0;
    while (elapsed < totalMs) {
        if (localAbort.load(std::memory_order_acquire) ||
            (cancelFlag && cancelFlag->load(std::memory_order_acquire))) {
            return false;
        }
        const int step = (std::min)(kTransientRetrySliceMs, totalMs - elapsed);
        std::this_thread::sleep_for(std::chrono::milliseconds(step));
        elapsed += step;
    }
    return !localAbort.load(std::memory_order_acquire) &&
           !(cancelFlag && cancelFlag->load(std::memory_order_acquire));
}

std::string trimAscii(std::string_view sv);

/// 弹性哨兵拆段：容忍翻译模型在哨兵周围插入空格或将方括号转为全角 【】，大幅降低整批退化为逐条串行的概率
std::vector<std::string> splitByFlexibleSentinel(std::string_view mergedOut) {
    std::vector<std::string> parts;
    size_t pos = 0;
    const size_t coreLen = std::strlen(kGoogleBatchSentinelCore);
    while (pos <= mergedOut.size()) {
        const size_t corePos = mergedOut.find(kGoogleBatchSentinelCore, pos);
        if (corePos == std::string_view::npos) {
            parts.push_back(trimAscii(mergedOut.substr(pos)));
            break;
        }
        // 向左剥离 '[' / '【' / 空白
        size_t segEnd = corePos;
        while (segEnd > pos) {
            if (mergedOut[segEnd - 1] == '[' || mergedOut[segEnd - 1] == ' ' || mergedOut[segEnd - 1] == '\t') {
                --segEnd;
            } else if (segEnd >= pos + 3 &&
                       static_cast<unsigned char>(mergedOut[segEnd - 3]) == 0xE3 &&
                       static_cast<unsigned char>(mergedOut[segEnd - 2]) == 0x80 &&
                       static_cast<unsigned char>(mergedOut[segEnd - 1]) == 0x90) { // UTF-8 '【'
                segEnd -= 3;
            } else {
                break;
            }
        }
        parts.push_back(trimAscii(mergedOut.substr(pos, segEnd - pos)));

        // 向右剥离 ']' / '】' / 空白
        size_t nextStart = corePos + coreLen;
        while (nextStart < mergedOut.size()) {
            if (mergedOut[nextStart] == ']' || mergedOut[nextStart] == ' ' || mergedOut[nextStart] == '\t') {
                ++nextStart;
            } else if (nextStart + 3 <= mergedOut.size() &&
                       static_cast<unsigned char>(mergedOut[nextStart]) == 0xE3 &&
                       static_cast<unsigned char>(mergedOut[nextStart + 1]) == 0x80 &&
                       static_cast<unsigned char>(mergedOut[nextStart + 2]) == 0x91) { // UTF-8 '】'
                nextStart += 3;
            } else {
                break;
            }
        }
        pos = nextStart;
    }
    return parts;
}

/// 受限并发批处理器（支持按渠道定制安全并发度 + 共享早停标志），0 常驻线程
template <typename BatchFn>
void runBatchesConcurrently(
    size_t batchCount,
    size_t maxConcurrency,
    const std::atomic<bool>* externalCancel,
    std::atomic<bool>& localAbort,
    BatchFn&& workerFn) {
    if (batchCount == 0) return;
    if (batchCount == 1 || maxConcurrency <= 1) {
        for (size_t i = 0; i < batchCount; ++i) {
            if (localAbort.load(std::memory_order_acquire) ||
                (externalCancel && externalCancel->load(std::memory_order_acquire))) {
                break;
            }
            workerFn(i, localAbort);
        }
        return;
    }
    const size_t threadCount = (std::min)(batchCount, (std::min)(maxConcurrency, kMaxConcurrentTranslateBatches));
    std::atomic<size_t> nextBatch{0};
    std::vector<std::thread> workers;
    workers.reserve(threadCount);
    for (size_t t = 0; t < threadCount; ++t) {
        workers.emplace_back([&]() {
            while (!localAbort.load(std::memory_order_acquire) &&
                   !(externalCancel && externalCancel->load(std::memory_order_acquire))) {
                const size_t idx = nextBatch.fetch_add(1, std::memory_order_acq_rel);
                if (idx >= batchCount) break;
                workerFn(idx, localAbort);
            }
        });
    }
    for (auto& th : workers) {
        if (th.joinable()) th.join();
    }
}

/// 轻量级会话 LRU 翻译缓存（上限 512 条，峰值 < 128 KB），加速重复 UI 词与二次贴图瞬译
class SessionTranslationCache {
public:
    static SessionTranslationCache& instance() {
        static SessionTranslationCache inst;
        return inst;
    }

    bool lookup(const std::string& key, std::string& outVal) {
        std::lock_guard<std::mutex> lock(m_mutex);
        auto it = m_map.find(key);
        if (it == m_map.end()) return false;
        m_lru.splice(m_lru.begin(), m_lru, it->second);
        outVal = it->second->second;
        return true;
    }

    void put(const std::string& key, const std::string& val) {
        if (key.empty() || val.empty() || key.size() > 1024 || val.size() > 2048) return;
        std::lock_guard<std::mutex> lock(m_mutex);
        auto it = m_map.find(key);
        if (it != m_map.end()) {
            it->second->second = val;
            m_lru.splice(m_lru.begin(), m_lru, it->second);
            return;
        }
        m_lru.emplace_front(key, val);
        m_map[key] = m_lru.begin();
        while (m_lru.size() > kMaxSessionCacheEntries) {
            m_map.erase(m_lru.back().first);
            m_lru.pop_back();
        }
    }

private:
    std::mutex m_mutex;
    std::list<std::pair<std::string, std::string>> m_lru;
    std::unordered_map<std::string, std::list<std::pair<std::string, std::string>>::iterator> m_map;
};

/// 判断单条文本是否已明显属于目标语种（例如 Postman 中译场景下的 "$batch-UDTs批量新增表"），免发多余请求
bool textAlreadyInTargetLanguage(
    std::string_view text,
    std::string_view targetLang,
    bool batchHasHan) noexcept {
    if (text.empty()) return true;
    const std::wstring ws = WinUtils::utf8ToWstring(std::string(text));
    size_t han = 0, kana = 0, hangul = 0, cyrillic = 0, latin = 0, nonAsciiLatin = 0;
    for (wchar_t ch : ws) {
        const uint32_t cp = static_cast<uint32_t>(ch);
        if ((cp >= 0x4E00 && cp <= 0x9FFF) || (cp >= 0x3400 && cp <= 0x4DBF)) ++han;
        else if ((cp >= 0x3040 && cp <= 0x30FF) || (cp >= 0x31F0 && cp <= 0x31FF)) ++kana;
        else if ((cp >= 0xAC00 && cp <= 0xD7AF) || (cp >= 0x1100 && cp <= 0x11FF)) ++hangul;
        else if (cp >= 0x0400 && cp <= 0x04FF) ++cyrillic;
        else if ((cp >= 'A' && cp <= 'Z') || (cp >= 'a' && cp <= 'z')) ++latin;
        else if (cp >= 0x00C0 && cp <= 0x024F) ++nonAsciiLatin;
    }
    const size_t total = han + kana + hangul + cyrillic + latin + nonAsciiLatin;
    if (total == 0) return true;
    if (targetLang == "zh-CN" || targetLang == "zh-TW") {
        // 已包含汉字且无日/韩/俄文：短技术标签（<=32字且含>=2汉字，如 "$batch-UDTs批量新增表"）或汉字比例足够直接保留
        if (han >= 2 && kana == 0 && hangul == 0 && cyrillic == 0 &&
            (ws.size() <= 32 || han * 3 >= latin)) {
            return true;
        }
    } else if (targetLang == "en") {
        // 仅当整批属于中文译英场景时，跳过其中纯 ASCII 英文标签（防止误伤法/德/西/葡手动翻英文）
        if (batchHasHan && latin > 0 && nonAsciiLatin == 0 &&
            han == 0 && kana == 0 && hangul == 0 && cyrillic == 0) {
            return true;
        }
    }
    return false;
}

int64_t unixNowSeconds() noexcept {
    return static_cast<int64_t>(std::chrono::duration_cast<std::chrono::seconds>(
        std::chrono::system_clock::now().time_since_epoch()).count());
}

std::string trimAscii(std::string_view sv) {
    size_t start = 0;
    while (start < sv.size() && std::isspace(static_cast<unsigned char>(sv[start]))) ++start;
    size_t end = sv.size();
    while (end > start && std::isspace(static_cast<unsigned char>(sv[end - 1]))) --end;
    return std::string(sv.substr(start, end - start));
}

std::string toLowerAscii(std::string_view sv) {
    std::string out(sv);
    std::transform(out.begin(), out.end(), out.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    return out;
}

bool startsWithNoCase(std::string_view text, std::string_view prefix) noexcept {
    if (text.size() < prefix.size()) return false;
    for (size_t i = 0; i < prefix.size(); ++i) {
        if (std::tolower(static_cast<unsigned char>(text[i])) !=
            std::tolower(static_cast<unsigned char>(prefix[i]))) {
            return false;
        }
    }
    return true;
}

bool endsWithNoCase(std::string_view text, std::string_view suffix) noexcept {
    if (text.size() < suffix.size()) return false;
    const size_t offset = text.size() - suffix.size();
    for (size_t i = 0; i < suffix.size(); ++i) {
        if (std::tolower(static_cast<unsigned char>(text[offset + i])) !=
            std::tolower(static_cast<unsigned char>(suffix[i]))) {
            return false;
        }
    }
    return true;
}

std::string percentEncodeUtf8(std::string_view input) {
    static constexpr char kHex[] = "0123456789ABCDEF";
    std::string out;
    out.reserve(input.size() * 2);
    for (unsigned char c : input) {
        if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
            (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.' || c == '~') {
            out.push_back(static_cast<char>(c));
        } else {
            out.push_back('%');
            out.push_back(kHex[(c >> 4) & 0x0F]);
            out.push_back(kHex[c & 0x0F]);
        }
    }
    return out;
}

struct ParsedHttpUrl {
    bool valid = false;
    bool isHttps = true;
    INTERNET_PORT port = INTERNET_DEFAULT_HTTPS_PORT;
    std::wstring host;
    std::wstring pathAndQuery;
};

ParsedHttpUrl crackUrl(std::string_view utf8Url) {
    ParsedHttpUrl res{};
    const std::wstring wUrl = WinUtils::utf8ToWstring(trimAscii(utf8Url));
    if (wUrl.empty()) return res;

    URL_COMPONENTS uc{};
    uc.dwStructSize = sizeof(uc);
    uc.dwHostNameLength = static_cast<DWORD>(-1);
    uc.dwUrlPathLength = static_cast<DWORD>(-1);
    uc.dwExtraInfoLength = static_cast<DWORD>(-1);

    if (!WinHttpCrackUrl(wUrl.c_str(), static_cast<DWORD>(wUrl.size()), 0, &uc)) {
        return res;
    }
    if (uc.nScheme != INTERNET_SCHEME_HTTPS && uc.nScheme != INTERNET_SCHEME_HTTP) {
        return res;
    }
    if (!uc.lpszHostName || uc.dwHostNameLength == 0) {
        return res;
    }

    res.valid = true;
    res.isHttps = (uc.nScheme == INTERNET_SCHEME_HTTPS);
    res.port = uc.nPort;
    res.host.assign(uc.lpszHostName, uc.dwHostNameLength);
    if (uc.lpszUrlPath && uc.dwUrlPathLength > 0) {
        res.pathAndQuery.assign(uc.lpszUrlPath, uc.dwUrlPathLength);
    } else {
        res.pathAndQuery = L"/";
    }
    if (uc.lpszExtraInfo && uc.dwExtraInfoLength > 0) {
        res.pathAndQuery.append(uc.lpszExtraInfo, uc.dwExtraInfoLength);
    }
    return res;
}

struct WinHttpResult {
    bool transportOk = false;
    bool cancelled = false;
    DWORD statusCode = 0;
    DWORD win32Error = 0;
    std::string body;
};

WinHttpResult executeHttpRequest(
    const ParsedHttpUrl& url,
    const wchar_t* method,
    const std::wstring& headers,
    const std::string& requestBody,
    int timeoutMs,
    const std::atomic<bool>* cancelFlag,
    const wchar_t* userAgentOverride = nullptr) {
    WinHttpResult result{};
    if (!url.valid) {
        result.win32Error = ERROR_INVALID_PARAMETER;
        return result;
    }
    if (cancelFlag && cancelFlag->load(std::memory_order_acquire)) {
        result.cancelled = true;
        return result;
    }

    const int clampedTimeout = std::clamp(
        timeoutMs, TranslationConstants::kMinTimeoutMs, TranslationConstants::kMaxTimeoutMs);
    const wchar_t* effectiveUa = (userAgentOverride && userAgentOverride[0] != L'\0')
        ? userAgentOverride
        : kBrowserUserAgent;

    HINTERNET session = WinHttpOpen(
        effectiveUa,
        WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,
        WINHTTP_NO_PROXY_NAME,
        WINHTTP_NO_PROXY_BYPASS,
        0);
    if (!session) {
        session = WinHttpOpen(
            effectiveUa,
            WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
            WINHTTP_NO_PROXY_NAME,
            WINHTTP_NO_PROXY_BYPASS,
            0);
    }
    if (!session) {
        result.win32Error = GetLastError();
        return result;
    }

    WinHttpSetTimeouts(session, clampedTimeout, clampedTimeout, clampedTimeout, clampedTimeout);

    HINTERNET connection = WinHttpConnect(session, url.host.c_str(), url.port, 0);
    HINTERNET request = nullptr;
    if (connection && !(cancelFlag && cancelFlag->load(std::memory_order_acquire))) {
        const DWORD flags = url.isHttps ? WINHTTP_FLAG_SECURE : 0;
        request = WinHttpOpenRequest(
            connection,
            method,
            url.pathAndQuery.c_str(),
            nullptr,
            WINHTTP_NO_REFERER,
            WINHTTP_DEFAULT_ACCEPT_TYPES,
            flags);
    }

    if (request) {
        const wchar_t* hdrPtr = headers.empty() ? WINHTTP_NO_ADDITIONAL_HEADERS : headers.c_str();
        const DWORD hdrLen = headers.empty() ? 0 : static_cast<DWORD>(headers.size());
        LPVOID bodyPtr = requestBody.empty()
            ? WINHTTP_NO_REQUEST_DATA
            : const_cast<char*>(requestBody.data());
        const DWORD bodyLen = static_cast<DWORD>(requestBody.size());

        if (!(cancelFlag && cancelFlag->load(std::memory_order_acquire)) &&
            WinHttpSendRequest(request, hdrPtr, hdrLen, bodyPtr, bodyLen, bodyLen, 0) &&
            WinHttpReceiveResponse(request, nullptr)) {
            DWORD statusCode = 0;
            DWORD statusSize = sizeof(statusCode);
            WinHttpQueryHeaders(
                request,
                WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                WINHTTP_HEADER_NAME_BY_INDEX,
                &statusCode,
                &statusSize,
                WINHTTP_NO_HEADER_INDEX);
            result.statusCode = statusCode;
            result.transportOk = true;

            while (true) {
                if (cancelFlag && cancelFlag->load(std::memory_order_acquire)) {
                    result.cancelled = true;
                    result.transportOk = false;
                    break;
                }
                DWORD available = 0;
                if (!WinHttpQueryDataAvailable(request, &available)) {
                    result.win32Error = GetLastError();
                    result.transportOk = false;
                    break;
                }
                if (available == 0) break;
                if (result.body.size() + available > TranslationConstants::kMaxHttpResponseBytes) {
                    result.win32Error = ERROR_BUFFER_OVERFLOW;
                    result.transportOk = false;
                    break;
                }
                const size_t offset = result.body.size();
                result.body.resize(offset + available);
                DWORD bytesRead = 0;
                if (!WinHttpReadData(request, result.body.data() + offset, available, &bytesRead)) {
                    result.win32Error = GetLastError();
                    result.transportOk = false;
                    break;
                }
                result.body.resize(offset + bytesRead);
                if (bytesRead == 0) break;
            }
        } else {
            if (cancelFlag && cancelFlag->load(std::memory_order_acquire)) {
                result.cancelled = true;
            } else {
                result.win32Error = GetLastError();
            }
        }
    } else if (cancelFlag && cancelFlag->load(std::memory_order_acquire)) {
        result.cancelled = true;
    } else {
        result.win32Error = GetLastError();
    }

    if (request) WinHttpCloseHandle(request);
    if (connection) WinHttpCloseHandle(connection);
    WinHttpCloseHandle(session);
    return result;
}

std::string base64Encode(const uint8_t* data, size_t len) {
    if (!data || len == 0) return {};
    DWORD outLen = 0;
    if (!CryptBinaryToStringA(
            data, static_cast<DWORD>(len),
            CRYPT_STRING_BASE64 | CRYPT_STRING_NOCRLF,
            nullptr, &outLen) || outLen == 0) {
        return {};
    }
    std::string encoded(outLen, '\0');
    if (!CryptBinaryToStringA(
            data, static_cast<DWORD>(len),
            CRYPT_STRING_BASE64 | CRYPT_STRING_NOCRLF,
            encoded.data(), &outLen)) {
        return {};
    }
    while (!encoded.empty() && encoded.back() == '\0') encoded.pop_back();
    return encoded;
}

bool base64Decode(std::string_view input, std::vector<uint8_t>& outBytes) {
    outBytes.clear();
    if (input.empty()) return false;
    DWORD outLen = 0;
    if (!CryptStringToBinaryA(
            input.data(), static_cast<DWORD>(input.size()),
            CRYPT_STRING_BASE64,
            nullptr, &outLen, nullptr, nullptr) || outLen == 0) {
        return false;
    }
    outBytes.resize(outLen);
    if (!CryptStringToBinaryA(
            input.data(), static_cast<DWORD>(input.size()),
            CRYPT_STRING_BASE64,
            outBytes.data(), &outLen, nullptr, nullptr)) {
        outBytes.clear();
        return false;
    }
    outBytes.resize(outLen);
    return true;
}

struct ScriptStats {
    size_t hanCount = 0;
    size_t kanaCount = 0;
    size_t hangulCount = 0;
    size_t cyrillicCount = 0;
    size_t latinCount = 0;
    size_t totalLetters = 0;
};

ScriptStats analyzeScriptStats(const std::vector<std::string>& texts) {
    ScriptStats stats{};
    for (const auto& s : texts) {
        const std::wstring ws = WinUtils::utf8ToWstring(s);
        for (wchar_t ch : ws) {
            const uint32_t cp = static_cast<uint32_t>(ch);
            if ((cp >= 0x4E00 && cp <= 0x9FFF) || (cp >= 0x3400 && cp <= 0x4DBF)) {
                ++stats.hanCount;
                ++stats.totalLetters;
            } else if ((cp >= 0x3040 && cp <= 0x30FF) || (cp >= 0x31F0 && cp <= 0x31FF)) {
                ++stats.kanaCount;
                ++stats.totalLetters;
            } else if ((cp >= 0xAC00 && cp <= 0xD7AF) || (cp >= 0x1100 && cp <= 0x11FF)) {
                ++stats.hangulCount;
                ++stats.totalLetters;
            } else if (cp >= 0x0400 && cp <= 0x04FF) {
                ++stats.cyrillicCount;
                ++stats.totalLetters;
            } else if ((cp >= 'A' && cp <= 'Z') || (cp >= 'a' && cp <= 'z') ||
                       (cp >= 0x00C0 && cp <= 0x024F)) {
                ++stats.latinCount;
                ++stats.totalLetters;
            }
        }
    }
    return stats;
}

std::string stripReasoningThinkBlocks(std::string_view raw) {
    std::string text = trimAscii(raw);
    if (text.empty()) return {};

    // 剥离推理模型偶发输出的 <think>...</think> 或 <thought>...</thought> 思考链，
    // 防止思考内容里的示例花括号干扰后续 JSON 负载提取。
    static constexpr std::array<std::pair<std::string_view, std::string_view>, 2> kTags = {{
        {"<think>", "</think>"},
        {"<thought>", "</thought>"}
    }};

    for (const auto& [openTag, closeTag] : kTags) {
        const std::string lower = toLowerAscii(text);
        const size_t closePos = lower.rfind(closeTag);
        if (closePos != std::string::npos) {
            std::string afterClose = trimAscii(text.substr(closePos + closeTag.size()));
            if (afterClose.empty()) {
                // 仅有思考链而无最终正文，绝不从思考内容中误提取 JSON 或原文
                return {};
            }
            text = std::move(afterClose);
            continue;
        }
        if (startsWithNoCase(text, openTag)) {
            // 未闭合的思考链（输出被 max_tokens 截断），返回空串交由调用方触发二分补译
            return {};
        }
    }
    return text;
}

std::string extractJsonPayloadFromModelContent(std::string_view raw) {
    std::string trimmed = stripReasoningThinkBlocks(raw);
    if (trimmed.empty()) return {};

    // 剥离模型偶发输出的 ```json ... ``` 围栏
    if (trimmed.rfind("```", 0) == 0) {
        const size_t firstNewline = trimmed.find('\n');
        if (firstNewline != std::string::npos) {
            trimmed = trimmed.substr(firstNewline + 1);
            const size_t closeFence = trimmed.rfind("```");
            if (closeFence != std::string::npos) {
                trimmed = trimmed.substr(0, closeFence);
            }
            trimmed = trimAscii(trimmed);
        }
    }

    const size_t objStart = trimmed.find('{');
    const size_t arrStart = trimmed.find('[');
    size_t start = std::string::npos;
    char closeChar = '}';
    if (objStart != std::string::npos && (arrStart == std::string::npos || objStart < arrStart)) {
        start = objStart;
        closeChar = '}';
    } else if (arrStart != std::string::npos) {
        start = arrStart;
        closeChar = ']';
    }
    if (start == std::string::npos) return trimmed;
    const size_t end = trimmed.rfind(closeChar);
    if (end == std::string::npos || end < start) return trimmed.substr(start);
    return trimmed.substr(start, end - start + 1);
}

size_t countUtf8CodepointsFast(std::string_view utf8) noexcept {
    size_t count = 0;
    for (unsigned char c : utf8) {
        if ((c & 0xC0u) != 0x80u) {
            ++count;
        }
    }
    return count;
}

std::vector<std::pair<size_t, size_t>> splitIntoBatches(
    const std::vector<std::string>& texts,
    size_t maxCharsPerBatch,
    size_t maxItemsPerBatch) {
    std::vector<std::pair<size_t, size_t>> ranges;
    size_t start = 0;
    while (start < texts.size()) {
        size_t chars = 0;
        size_t end = start;
        while (end < texts.size()) {
            // 按 Unicode 码点数计数（避免中文 UTF-8 3 字节/字被误拆成 3 倍碎片请求）
            const size_t nextChars = chars + countUtf8CodepointsFast(texts[end]) + 18;
            if (end > start && (nextChars > maxCharsPerBatch || (end - start) >= maxItemsPerBatch)) {
                break;
            }
            chars = nextChars;
            ++end;
        }
        ranges.emplace_back(start, end);
        start = end;
    }
    return ranges;
}

// ── 多态翻译提供方策略接口 (Strategy Pattern) ──────────────────────────────────

class ITranslationProvider {
public:
    virtual ~ITranslationProvider() = default;
    virtual const char* id() const noexcept = 0;
    virtual TranslationBatchResult translate(
        const std::vector<std::string>& texts,
        const std::string& effectiveTargetLang,
        const TranslationSettings& settings,
        const std::atomic<bool>* cancelFlag) = 0;
};

struct BingCredsSnapshot {
    std::string host;
    std::string ig;
    std::string iid;
    std::string key;
    std::string token;
    uint32_t reqSeq = 1;
};

class EdgeFreeProvider final : public ITranslationProvider {
public:
    using CredsAcquirer = std::function<bool(int, const std::atomic<bool>*, BingCredsSnapshot&, std::string&, std::string&)>;
    using CredsInvalidator = std::function<void()>;

    EdgeFreeProvider(CredsAcquirer acquirer, CredsInvalidator invalidator)
        : m_acquireCreds(std::move(acquirer)), m_invalidateCreds(std::move(invalidator)) {}

    const char* id() const noexcept override { return "edge_free"; }

    TranslationBatchResult translate(
        const std::vector<std::string>& texts,
        const std::string& effectiveTargetLang,
        const TranslationSettings& settings,
        const std::atomic<bool>* cancelFlag) override {
        TranslationBatchResult out{};
        out.providerUsed = id();
        out.effectiveTargetLang = effectiveTargetLang;
        out.translations.resize(texts.size());

        const std::string edgeLang = TranslationService::toEdgeLangCode(effectiveTargetLang);

        const int edgeTimeoutMs = (std::min)(settings.timeoutMs, 4500);

        auto flattenSingleLine = [](std::string_view s) -> std::string {
            std::string out;
            out.reserve(s.size());
            bool prevSpace = false;
            for (unsigned char c : s) {
                if (c == '\r' || c == '\n') {
                    if (!out.empty() && !prevSpace) {
                        out.push_back(' ');
                        prevSpace = true;
                    }
                } else {
                    out.push_back(static_cast<char>(c));
                    prevSpace = (c == ' ' || c == '\t');
                }
            }
            return trimAscii(out);
        };

        auto splitByNewline = [](std::string_view mergedOut) -> std::vector<std::string> {
            std::vector<std::string> lines;
            size_t pos = 0;
            while (pos <= mergedOut.size()) {
                const size_t nl = mergedOut.find('\n', pos);
                std::string_view seg = (nl == std::string_view::npos)
                    ? mergedOut.substr(pos)
                    : mergedOut.substr(pos, nl - pos);
                std::string cleaned = trimAscii(seg);
                if (!cleaned.empty()) {
                    lines.push_back(std::move(cleaned));
                }
                if (nl == std::string_view::npos) break;
                pos = nl + 1;
            }
            return lines;
        };

        auto translateSingle = [&](const std::string& src, std::string& dst, std::string& detected,
                                   std::string& errCode, std::string& errDetail) -> bool {
            for (int attempt = 0; attempt < 2; ++attempt) {
                BingCredsSnapshot creds{};
                if (!m_acquireCreds(edgeTimeoutMs, cancelFlag, creds, errCode, errDetail)) {
                    if (errCode.empty()) errCode = "network";
                    return false;
                }

                const std::string apiUrl =
                    "https://" + creds.host + "/ttranslatev3?isVertical=1&IG=" +
                    percentEncodeUtf8(creds.ig) + "&IID=" + percentEncodeUtf8(creds.iid) + "." +
                    std::to_string(creds.reqSeq);
                const ParsedHttpUrl parsed = crackUrl(apiUrl);
                const std::string formBody =
                    "fromLang=auto-detect&to=" + percentEncodeUtf8(edgeLang) +
                    "&text=" + percentEncodeUtf8(src) +
                    "&token=" + percentEncodeUtf8(creds.token) +
                    "&key=" + percentEncodeUtf8(creds.key);
                const std::wstring wHost = WinUtils::utf8ToWstring(creds.host);
                const std::wstring headers =
                    L"Content-Type: application/x-www-form-urlencoded; charset=utf-8\r\n"
                    L"Referer: https://" + wHost + L"/translator\r\n"
                    L"Origin: https://" + wHost + L"\r\n";

                const WinHttpResult http = executeHttpRequest(
                    parsed, L"POST", headers, formBody, edgeTimeoutMs, cancelFlag);
                if (http.cancelled) {
                    errCode = "cancelled";
                    return false;
                }
                if (!http.transportOk) {
                    errCode = "network";
                    errDetail = "winhttp_error=" + std::to_string(http.win32Error);
                    return false;
                }
                if (http.statusCode == 429) {
                    errCode = "rate_limited";
                    errDetail = "bing_http_429";
                    return false;
                }
                if (http.statusCode == 401 || http.statusCode == 403) {
                    m_invalidateCreds();
                    errCode = "http_error";
                    errDetail = "bing_http_" + std::to_string(http.statusCode);
                    continue;
                }
                if (http.statusCode < 200 || http.statusCode >= 300) {
                    errCode = "http_error";
                    errDetail = "bing_http_" + std::to_string(http.statusCode);
                    return false;
                }

                std::vector<std::string> singleOut;
                if (TranslationService::parseEdgeTranslateResponse(http.body, 1, singleOut, detected) &&
                    !singleOut.empty()) {
                    dst = std::move(singleOut[0]);
                    return true;
                }

                // Bing 令牌过期时可能返回 200 + {"statusCode":205}：清缓存并重试一次
                if (attempt == 0) {
                    m_invalidateCreds();
                    continue;
                }
                errCode = "invalid_response";
                errDetail = "bing_parse_failed";
                return false;
            }
            return false;
        };

        // 并发前先单线程预热一次 Bing 会话凭据，杜绝冷启多线程重复下载 600KB HTML 引发惊群与 205 风控
        {
            BingCredsSnapshot warmupCreds{};
            std::string warmErrCode;
            std::string warmErrDetail;
            if (!m_acquireCreds(settings.timeoutMs, cancelFlag, warmupCreds, warmErrCode, warmErrDetail)) {
                out.errorCode = warmErrCode.empty() ? "network" : std::move(warmErrCode);
                out.errorDetail = std::move(warmErrDetail);
                return out;
            }
        }

        // Bing ttranslatev3 单次上限约 1000 Unicode 字符；按码点数控在 920 字符 / 14 段，温和 2 路并发防 205 风控
        const auto batches = splitIntoBatches(texts, 920, 14);
        std::atomic<bool> abortFlag{false};
        std::mutex errMutex;
        std::string firstErrCode;
        std::string firstErrDetail;
        std::string firstDetected;

        auto recordFailure = [&](const std::string& ec, const std::string& ed) {
            abortFlag.store(true, std::memory_order_release);
            std::lock_guard<std::mutex> lock(errMutex);
            if (firstErrCode.empty()) {
                firstErrCode = ec;
                firstErrDetail = ed;
            }
        };

        runBatchesConcurrently(batches.size(), /*maxConcurrency=*/2, cancelFlag, abortFlag, [&](size_t bIdx, std::atomic<bool>& localAbort) {
            const auto [beginIdx, endIdx] = batches[bIdx];
            const size_t count = endIdx - beginIdx;
            std::string errCode;
            std::string errDetail;
            std::string detected;

            if (count == 1) {
                std::string translated;
                if (!translateSingle(texts[beginIdx], translated, detected, errCode, errDetail)) {
                    recordFailure(errCode, errDetail);
                    return;
                }
                if (!detected.empty()) {
                    std::lock_guard<std::mutex> lock(errMutex);
                    if (firstDetected.empty()) firstDetected = detected;
                }
                out.translations[beginIdx] = std::move(translated);
                return;
            }

            // 使用纯换行符 '\n' 分段（实测 Bing LLM 1.4s 极速且不触发哨兵吞字）；发前压平每条内换行确保严格 1:1
            std::string merged;
            for (size_t i = beginIdx; i < endIdx; ++i) {
                if (i > beginIdx) merged.push_back('\n');
                merged += flattenSingleLine(texts[i]);
            }

            std::string mergedOut;
            if (!translateSingle(merged, mergedOut, detected, errCode, errDetail)) {
                recordFailure(errCode, errDetail);
                return;
            }
            if (!detected.empty()) {
                std::lock_guard<std::mutex> lock(errMutex);
                if (firstDetected.empty()) firstDetected = detected;
            }

            std::vector<std::string> parts = splitByNewline(mergedOut);
            if (parts.size() == count) {
                for (size_t i = 0; i < count; ++i) {
                    out.translations[beginIdx + i] = std::move(parts[i]);
                }
            } else if (count <= 3) {
                for (size_t i = beginIdx; i < endIdx; ++i) {
                    if (localAbort.load(std::memory_order_acquire) ||
                        (cancelFlag && cancelFlag->load(std::memory_order_acquire))) {
                        return;
                    }
                    std::string singleOut;
                    std::string singleDet;
                    if (!translateSingle(texts[i], singleOut, singleDet, errCode, errDetail)) {
                        recordFailure(errCode, errDetail);
                        return;
                    }
                    out.translations[i] = std::move(singleOut);
                }
            } else {
                // 大批返回行数不对齐时绝不退化为 14 次逐条串行空烧 20 秒，立即快速止损切给备用极速通道
                recordFailure("invalid_response", "bing_line_mismatch");
                return;
            }
        });

        if (!firstErrCode.empty()) {
            out.errorCode = std::move(firstErrCode);
            out.errorDetail = std::move(firstErrDetail);
            return out;
        }
        if (cancelFlag && cancelFlag->load(std::memory_order_acquire)) {
            out.errorCode = "cancelled";
            return out;
        }
        out.detectedSourceLang = std::move(firstDetected);
        out.success = true;
        return out;
    }

private:
    CredsAcquirer m_acquireCreds;
    CredsInvalidator m_invalidateCreds;
};

class GoogleFreeProvider final : public ITranslationProvider {
public:
    const char* id() const noexcept override { return "google_free"; }

    TranslationBatchResult translate(
        const std::vector<std::string>& texts,
        const std::string& effectiveTargetLang,
        const TranslationSettings& settings,
        const std::atomic<bool>* cancelFlag) override {
        TranslationBatchResult out{};
        out.providerUsed = id();
        out.effectiveTargetLang = effectiveTargetLang;
        out.translations.resize(texts.size());

        const std::string gLang = TranslationService::toGoogleLangCode(effectiveTargetLang);
        const std::string endpointUrl =
            "https://translate.googleapis.com/translate_a/single?client=gtx&sl=auto&tl=" +
            percentEncodeUtf8(gLang) + "&dt=t";
        const ParsedHttpUrl parsed = crackUrl(endpointUrl);

        auto translateSingle = [&](const std::string& src, std::string& dst, std::string& detected,
                                   std::string& errCode, std::string& errDetail) -> bool {
            const std::string formBody = "q=" + percentEncodeUtf8(src);
            const std::wstring headers = L"Content-Type: application/x-www-form-urlencoded; charset=utf-8\r\n";
            const WinHttpResult http = executeHttpRequest(
                parsed, L"POST", headers, formBody, settings.timeoutMs, cancelFlag);
            if (http.cancelled) {
                errCode = "cancelled";
                return false;
            }
            if (!http.transportOk) {
                errCode = "network";
                errDetail = "winhttp_error=" + std::to_string(http.win32Error);
                return false;
            }
            if (http.statusCode == 429) {
                errCode = "rate_limited";
                errDetail = "google_http_429";
                return false;
            }
            if (http.statusCode < 200 || http.statusCode >= 300) {
                errCode = "http_error";
                errDetail = "google_http_" + std::to_string(http.statusCode);
                return false;
            }
            if (!TranslationService::parseGoogleTranslateResponse(http.body, dst, detected)) {
                errCode = "invalid_response";
                errDetail = "google_parse_failed";
                return false;
            }
            return true;
        };

        const auto batches = splitIntoBatches(texts, 4200, 18);
        std::atomic<bool> abortFlag{false};
        std::mutex errMutex;
        std::string firstErrCode;
        std::string firstErrDetail;
        std::string firstDetected;

        auto recordFailure = [&](const std::string& ec, const std::string& ed) {
            abortFlag.store(true, std::memory_order_release);
            std::lock_guard<std::mutex> lock(errMutex);
            if (firstErrCode.empty()) {
                firstErrCode = ec;
                firstErrDetail = ed;
            }
        };

        runBatchesConcurrently(batches.size(), kMaxConcurrentTranslateBatches, cancelFlag, abortFlag, [&](size_t bIdx, std::atomic<bool>& localAbort) {
            const auto [beginIdx, endIdx] = batches[bIdx];
            const size_t count = endIdx - beginIdx;
            std::string errCode;
            std::string errDetail;
            std::string detected;

            if (count == 1) {
                std::string translated;
                if (!translateSingle(texts[beginIdx], translated, detected, errCode, errDetail)) {
                    recordFailure(errCode, errDetail);
                    return;
                }
                if (!detected.empty()) {
                    std::lock_guard<std::mutex> lock(errMutex);
                    if (firstDetected.empty()) firstDetected = detected;
                }
                out.translations[beginIdx] = std::move(translated);
                return;
            }

            std::string merged;
            for (size_t i = beginIdx; i < endIdx; ++i) {
                if (i > beginIdx) merged += kGoogleBatchSentinel;
                merged += texts[i];
            }

            std::string mergedOut;
            if (!translateSingle(merged, mergedOut, detected, errCode, errDetail)) {
                recordFailure(errCode, errDetail);
                return;
            }
            if (!detected.empty()) {
                std::lock_guard<std::mutex> lock(errMutex);
                if (firstDetected.empty()) firstDetected = detected;
            }

            std::vector<std::string> parts = splitByFlexibleSentinel(mergedOut);
            if (parts.size() == count) {
                for (size_t i = 0; i < count; ++i) {
                    out.translations[beginIdx + i] = std::move(parts[i]);
                }
            } else {
                for (size_t i = beginIdx; i < endIdx; ++i) {
                    if (localAbort.load(std::memory_order_acquire) ||
                        (cancelFlag && cancelFlag->load(std::memory_order_acquire))) {
                        return;
                    }
                    std::string singleOut;
                    std::string singleDet;
                    if (!translateSingle(texts[i], singleOut, singleDet, errCode, errDetail)) {
                        recordFailure(errCode, errDetail);
                        return;
                    }
                    out.translations[i] = std::move(singleOut);
                }
            }
        });

        if (!firstErrCode.empty()) {
            out.errorCode = std::move(firstErrCode);
            out.errorDetail = std::move(firstErrDetail);
            return out;
        }
        if (cancelFlag && cancelFlag->load(std::memory_order_acquire)) {
            out.errorCode = "cancelled";
            return out;
        }
        out.detectedSourceLang = std::move(firstDetected);
        out.success = true;
        return out;
    }
};

class OpenAiCompatibleProvider final : public ITranslationProvider {
public:
    const char* id() const noexcept override { return "openai_compatible"; }

    TranslationBatchResult translate(
        const std::vector<std::string>& texts,
        const std::string& effectiveTargetLang,
        const TranslationSettings& settings,
        const std::atomic<bool>* cancelFlag) override {
        TranslationBatchResult out{};
        out.providerUsed = id();
        out.effectiveTargetLang = effectiveTargetLang;
        out.translations.resize(texts.size());

        std::string fullUrl;
        if (!TranslationService::normalizeOpenAiChatUrl(settings.openaiBaseUrl, fullUrl)) {
            out.errorCode = "invalid_endpoint";
            out.errorDetail = "Invalid OpenAI-compatible base URL";
            return out;
        }
        const bool isLocal = TranslationService::isLoopbackHttpUrl(fullUrl);
        const bool isKeylessPublic = TranslationService::isKeylessPublicOpenAiUrl(fullUrl);
        if (isKeylessPublic) {
            out.providerUsed = "index_translate";
        }
        if (settings.openaiApiKey.empty() && !isLocal && !isKeylessPublic) {
            out.errorCode = "missing_api_key";
            out.errorDetail = "API key is required for non-local OpenAI-compatible endpoints";
            return out;
        }
        const std::string model = trimAscii(settings.openaiModel).empty()
            ? std::string(isKeylessPublic
                ? TranslationConstants::kIndexTranslateDefaultModel
                : TranslationConstants::kDefaultOpenAiModel)
            : trimAscii(settings.openaiModel);

        const ParsedHttpUrl parsed = crackUrl(fullUrl);
        if (!parsed.valid) {
            out.errorCode = "invalid_endpoint";
            out.errorDetail = "Cannot parse OpenAI-compatible URL";
            return out;
        }

        const std::string targetHuman = TranslationService::toHumanLanguageName(effectiveTargetLang);
        const std::string systemPrompt =
            "You are a precise UI and screenshot text translator. "
            "Translate each item in the input JSON array into " + targetHuman + ". "
            "Preserve code identifiers, numbers, URLs, and concise UI phrasing. "
            "Return ONLY valid JSON in the exact schema: "
            "{\"translations\":[{\"id\":0,\"text\":\"...\"}]}. "
            "Do not add explanations or markdown fences.";

        std::wstring headers = L"Content-Type: application/json; charset=utf-8\r\n";
        // 安全防线：内置免 Key 公网端点绝不透传用户历史保存的私有 API Key
        if (!isKeylessPublic && !settings.openaiApiKey.empty()) {
            headers += L"Authorization: Bearer " + WinUtils::utf8ToWstring(settings.openaiApiKey) + L"\r\n";
        }

        const OpenAiBatchPolicy policy = TranslationService::resolveOpenAiBatchPolicy(
            fullUrl, model, settings.timeoutMs);
        const wchar_t* effectiveUa = isKeylessPublic ? kIndexTranslateUserAgent : kApiClientUserAgent;
        const auto batches = splitIntoBatches(texts, policy.maxCharsPerBatch, policy.maxItemsPerBatch);

        std::atomic<bool> abortFlag{false};
        std::atomic<size_t> translatedItemCount{0};
        std::mutex errMutex;
        std::string firstFatalErrCode;
        std::string firstFatalErrDetail;
        std::string firstSoftErrCode;
        std::string firstSoftErrDetail;

        auto recordFatal = [&](const std::string& ec, const std::string& ed) {
            abortFlag.store(true, std::memory_order_release);
            std::lock_guard<std::mutex> lock(errMutex);
            if (firstFatalErrCode.empty()) {
                firstFatalErrCode = ec;
                firstFatalErrDetail = ed;
            }
        };
        auto recordSoft = [&](const std::string& ec, const std::string& ed) {
            std::lock_guard<std::mutex> lock(errMutex);
            if (firstSoftErrCode.empty()) {
                firstSoftErrCode = ec;
                firstSoftErrDetail = ed;
            }
        };

        std::function<void(size_t, size_t, int, std::atomic<bool>&)> translateRange;
        translateRange = [&](size_t beginIdx, size_t endIdx, int depth, std::atomic<bool>& localAbort) {
            if (beginIdx >= endIdx) return;
            if (localAbort.load(std::memory_order_acquire) ||
                (cancelFlag && cancelFlag->load(std::memory_order_acquire))) {
                return;
            }

            const size_t count = endIdx - beginIdx;
            json items = json::array();
            for (size_t i = beginIdx; i < endIdx; ++i) {
                items.push_back(json{{"id", static_cast<int>(i - beginIdx)}, {"text", texts[i]}});
            }
            json reqJson = {
                {"model", model},
                {"temperature", isKeylessPublic ? 0.0 : 0.1},
                {"messages", json::array({
                    json{{"role", "system"}, {"content", systemPrompt}},
                    json{{"role", "user"}, {"content", json{{"items", items}}.dump()}}
                })}
            };
            if (isKeylessPublic) {
                // 对齐 Index-Translate 官方推荐参数，显式关闭思考链以降低延迟并避免思维块污染 JSON 输出
                reqJson["chat_template_kwargs"] = json{{"enable_thinking", false}};
            }
            const std::string payload = reqJson.dump();

            WinHttpResult http{};
            for (int attempt = 0; attempt < 2; ++attempt) {
                http = executeHttpRequest(
                    parsed, L"POST", headers, payload, policy.effectiveTimeoutMs, cancelFlag, effectiveUa);
                if (http.cancelled) {
                    recordFatal("cancelled", "");
                    return;
                }
                const bool isTimeout = (!http.transportOk && http.win32Error == ERROR_WINHTTP_TIMEOUT);
                const bool isTransientHttp = (http.transportOk &&
                    (http.statusCode == 429 || http.statusCode == 500 ||
                     http.statusCode == 502 || http.statusCode == 503 || http.statusCode == 504));
                if ((isTimeout || isTransientHttp) && attempt == 0) {
                    LOG_WARN("TranslationService: {} transient error (status={}, win32={}), retrying once (items={}, model={})",
                             out.providerUsed, http.statusCode, http.win32Error, count, model);
                    if (!sleepCancelAwareMs(kTransientRetryDelayMs, cancelFlag, localAbort)) {
                        if (cancelFlag && cancelFlag->load(std::memory_order_acquire)) {
                            recordFatal("cancelled", "");
                        }
                        return;
                    }
                    continue;
                }
                break;
            }

            if (!http.transportOk) {
                if (http.win32Error == ERROR_WINHTTP_TIMEOUT && count > 1 && depth < kMaxBinarySplitDepth) {
                    const size_t mid = beginIdx + count / 2;
                    LOG_WARN("TranslationService: {} batch timeout (items={}, depth={}), splitting",
                             out.providerUsed, count, depth);
                    translateRange(beginIdx, mid, depth + 1, localAbort);
                    translateRange(mid, endIdx, depth + 1, localAbort);
                    return;
                }
                recordFatal("network", "winhttp_error=" + std::to_string(http.win32Error));
                return;
            }

            if (http.statusCode == 401 || http.statusCode == 403) {
                recordFatal("auth_failed", "openai_http_" + std::to_string(http.statusCode));
                return;
            }
            if (http.statusCode == 429) {
                recordFatal("rate_limited", "openai_http_429");
                return;
            }
            if (http.statusCode < 200 || http.statusCode >= 300) {
                const bool isSplittableRejection =
                    (http.statusCode == 400 || http.statusCode == 412 ||
                     http.statusCode == 413 || http.statusCode == 422 ||
                     http.statusCode == 451);
                if (isSplittableRejection && count > 1 && depth < kMaxBinarySplitDepth) {
                    const size_t mid = beginIdx + count / 2;
                    LOG_WARN("TranslationService: {} upstream rejected batch (status={}, items={}, depth={}), splitting",
                             out.providerUsed, http.statusCode, count, depth);
                    translateRange(beginIdx, mid, depth + 1, localAbort);
                    translateRange(mid, endIdx, depth + 1, localAbort);
                    return;
                }
                if (isSplittableRejection && count == 1 && texts.size() > 1) {
                    out.translations[beginIdx] = texts[beginIdx];
                    recordSoft("http_error", "openai_http_" + std::to_string(http.statusCode));
                    LOG_WARN("TranslationService: {} preserved single rejected item verbatim (status={}, model={})",
                             out.providerUsed, http.statusCode, model);
                    return;
                }
                recordFatal("http_error", "openai_http_" + std::to_string(http.statusCode));
                return;
            }

            std::vector<std::string> chunkTranslations;
            if (!TranslationService::parseOpenAiBatchResponse(http.body, count, chunkTranslations)) {
                if (count > 1 && depth < kMaxBinarySplitDepth) {
                    const size_t mid = beginIdx + count / 2;
                    LOG_WARN("TranslationService: {} batch parse incomplete (items={}, depth={}), splitting",
                             out.providerUsed, count, depth);
                    translateRange(beginIdx, mid, depth + 1, localAbort);
                    translateRange(mid, endIdx, depth + 1, localAbort);
                    return;
                }
                if (count == 1 && texts.size() > 1) {
                    out.translations[beginIdx] = texts[beginIdx];
                    recordSoft("invalid_response", "openai_parse_failed");
                    LOG_WARN("TranslationService: {} preserved single unparseable item verbatim (model={})",
                             out.providerUsed, model);
                    return;
                }
                recordFatal("invalid_response", "openai_parse_failed");
                return;
            }

            for (size_t i = 0; i < count; ++i) {
                out.translations[beginIdx + i] = std::move(chunkTranslations[i]);
            }
            translatedItemCount.fetch_add(count, std::memory_order_relaxed);
        };

        runBatchesConcurrently(
            batches.size(), policy.maxConcurrency, cancelFlag, abortFlag,
            [&](size_t bIdx, std::atomic<bool>& localAbort) {
                translateRange(batches[bIdx].first, batches[bIdx].second, 0, localAbort);
            });

        if (!firstFatalErrCode.empty()) {
            out.errorCode = std::move(firstFatalErrCode);
            out.errorDetail = std::move(firstFatalErrDetail);
            return out;
        }
        if (cancelFlag && cancelFlag->load(std::memory_order_acquire)) {
            out.errorCode = "cancelled";
            return out;
        }
        if (translatedItemCount.load(std::memory_order_acquire) == 0 && !firstSoftErrCode.empty()) {
            out.errorCode = std::move(firstSoftErrCode);
            out.errorDetail = std::move(firstSoftErrDetail);
            return out;
        }
        for (size_t i = 0; i < texts.size(); ++i) {
            if (out.translations[i].empty() && !texts[i].empty()) {
                out.translations[i] = texts[i];
            }
        }

        out.success = true;
        return out;
    }
};

class DeepLProvider final : public ITranslationProvider {
public:
    const char* id() const noexcept override { return "deepl"; }

    TranslationBatchResult translate(
        const std::vector<std::string>& texts,
        const std::string& effectiveTargetLang,
        const TranslationSettings& settings,
        const std::atomic<bool>* cancelFlag) override {
        TranslationBatchResult out{};
        out.providerUsed = id();
        out.effectiveTargetLang = effectiveTargetLang;
        out.translations.resize(texts.size());

        std::string fullUrl;
        if (!TranslationService::normalizeDeepLUrl(settings.deeplBaseUrl, settings.deeplApiKey, fullUrl)) {
            out.errorCode = "invalid_endpoint";
            out.errorDetail = "Invalid DeepL endpoint URL";
            return out;
        }
        if (settings.deeplApiKey.empty() && !TranslationService::isLoopbackHttpUrl(fullUrl)) {
            out.errorCode = "missing_api_key";
            out.errorDetail = "DeepL API key is required";
            return out;
        }
        const ParsedHttpUrl parsed = crackUrl(fullUrl);
        if (!parsed.valid) {
            out.errorCode = "invalid_endpoint";
            out.errorDetail = "Cannot parse DeepL URL";
            return out;
        }

        const std::string deeplLang = TranslationService::toDeepLLangCode(effectiveTargetLang);
        std::wstring headers = L"Content-Type: application/json; charset=utf-8\r\n";
        if (!settings.deeplApiKey.empty()) {
            headers += L"Authorization: DeepL-Auth-Key " + WinUtils::utf8ToWstring(settings.deeplApiKey) + L"\r\n";
        }

        const auto batches = splitIntoBatches(texts, 16000, 40);
        std::atomic<bool> localAbort{false};
        for (const auto& [beginIdx, endIdx] : batches) {
            json arr = json::array();
            for (size_t i = beginIdx; i < endIdx; ++i) arr.push_back(texts[i]);
            const json reqJson = {
                {"text", arr},
                {"target_lang", deeplLang}
            };
            const std::string payload = reqJson.dump();
            WinHttpResult http{};
            for (int attempt = 0; attempt < 2; ++attempt) {
                http = executeHttpRequest(
                    parsed, L"POST", headers, payload, settings.timeoutMs, cancelFlag, kApiClientUserAgent);
                if (http.cancelled) {
                    out.errorCode = "cancelled";
                    return out;
                }
                const bool isRetryable =
                    (!http.transportOk && http.win32Error == ERROR_WINHTTP_TIMEOUT) ||
                    (http.transportOk && (http.statusCode == 429 || http.statusCode == 502 ||
                                          http.statusCode == 503 || http.statusCode == 504));
                if (isRetryable && attempt == 0) {
                    if (!sleepCancelAwareMs(kTransientRetryDelayMs, cancelFlag, localAbort)) {
                        out.errorCode = "cancelled";
                        return out;
                    }
                    continue;
                }
                break;
            }
            if (!http.transportOk) {
                out.errorCode = "network";
                out.errorDetail = "winhttp_error=" + std::to_string(http.win32Error);
                return out;
            }
            if (http.statusCode == 401 || http.statusCode == 403) {
                out.errorCode = "auth_failed";
                out.errorDetail = "deepl_http_" + std::to_string(http.statusCode);
                return out;
            }
            if (http.statusCode == 429 || http.statusCode == 456) {
                out.errorCode = "rate_limited";
                out.errorDetail = "deepl_http_" + std::to_string(http.statusCode);
                return out;
            }
            if (http.statusCode < 200 || http.statusCode >= 300) {
                out.errorCode = "http_error";
                out.errorDetail = "deepl_http_" + std::to_string(http.statusCode);
                return out;
            }

            std::vector<std::string> chunkTranslations;
            std::string detected;
            if (!TranslationService::parseDeepLTranslateResponse(
                    http.body, endIdx - beginIdx, chunkTranslations, detected)) {
                out.errorCode = "invalid_response";
                out.errorDetail = "deepl_parse_failed";
                return out;
            }
            if (out.detectedSourceLang.empty() && !detected.empty()) {
                out.detectedSourceLang = detected;
            }
            for (size_t i = 0; i < chunkTranslations.size(); ++i) {
                out.translations[beginIdx + i] = std::move(chunkTranslations[i]);
            }
        }

        out.success = true;
        return out;
    }
};

} // namespace

// ─────────────────────────────────────────────────────────────────────────────
// TranslationService 成员实现
// ─────────────────────────────────────────────────────────────────────────────

TranslationService& TranslationService::instance() {
    static TranslationService inst;
    return inst;
}

TranslationSettings TranslationService::loadSettings() const {
    auto& cfg = ConfigManager::instance();
    TranslationSettings s{};
    s.provider = cfg.get<std::string>("/translate/provider", TranslationConstants::kProviderAutoFree);
    if (s.provider != TranslationConstants::kProviderAutoFree &&
        s.provider != TranslationConstants::kProviderOpenAiCompatible &&
        s.provider != TranslationConstants::kProviderDeepL) {
        s.provider = TranslationConstants::kProviderAutoFree;
    }
    s.freePrimary = cfg.get<std::string>("/translate/freePrimary", TranslationConstants::kFreePrimaryEdge);
    if (s.freePrimary != TranslationConstants::kFreePrimaryEdge &&
        s.freePrimary != TranslationConstants::kFreePrimaryGoogle) {
        s.freePrimary = TranslationConstants::kFreePrimaryEdge;
    }
    s.targetLang = cfg.get<std::string>("/translate/targetLang", TranslationConstants::kTargetLangAuto);
    if (s.targetLang.empty()) s.targetLang = TranslationConstants::kTargetLangAuto;
    s.secondaryTargetLang = cfg.get<std::string>(
        "/translate/secondaryTargetLang", TranslationConstants::kDefaultSecondaryTargetLang);
    if (s.secondaryTargetLang.empty()) s.secondaryTargetLang = TranslationConstants::kDefaultSecondaryTargetLang;

    s.openaiBaseUrl = cfg.get<std::string>("/translate/openaiBaseUrl", TranslationConstants::kDefaultOpenAiBaseUrl);
    if (trimAscii(s.openaiBaseUrl).empty()) s.openaiBaseUrl = TranslationConstants::kDefaultOpenAiBaseUrl;
    s.openaiApiKey = unprotectSecretFromStorage(cfg.get<std::string>("/translate/openaiApiKey", ""));
    s.openaiModel = cfg.get<std::string>("/translate/openaiModel", TranslationConstants::kDefaultOpenAiModel);
    if (trimAscii(s.openaiModel).empty()) s.openaiModel = TranslationConstants::kDefaultOpenAiModel;

    s.deeplBaseUrl = cfg.get<std::string>("/translate/deeplBaseUrl", "");
    s.deeplApiKey = unprotectSecretFromStorage(cfg.get<std::string>("/translate/deeplApiKey", ""));

    s.timeoutMs = std::clamp(
        cfg.get<int>("/translate/timeoutMs", TranslationConstants::kDefaultTimeoutMs),
        TranslationConstants::kMinTimeoutMs,
        TranslationConstants::kMaxTimeoutMs);
    return s;
}

bool TranslationService::saveSettings(const TranslationSettings& settings) {
    auto& cfg = ConfigManager::instance();
    const std::string provider = (settings.provider == TranslationConstants::kProviderOpenAiCompatible ||
                                  settings.provider == TranslationConstants::kProviderDeepL)
        ? settings.provider
        : std::string(TranslationConstants::kProviderAutoFree);
    const std::string freePrimary = (settings.freePrimary == TranslationConstants::kFreePrimaryGoogle)
        ? std::string(TranslationConstants::kFreePrimaryGoogle)
        : std::string(TranslationConstants::kFreePrimaryEdge);
    const std::string targetLang = settings.targetLang.empty()
        ? std::string(TranslationConstants::kTargetLangAuto)
        : canonicalizeLanguageCode(settings.targetLang);
    const std::string secondaryLang = settings.secondaryTargetLang.empty()
        ? std::string(TranslationConstants::kDefaultSecondaryTargetLang)
        : canonicalizeLanguageCode(settings.secondaryTargetLang);
    const std::string openaiBase = trimAscii(settings.openaiBaseUrl).empty()
        ? std::string(TranslationConstants::kDefaultOpenAiBaseUrl)
        : trimAscii(settings.openaiBaseUrl);
    const std::string openaiModel = trimAscii(settings.openaiModel).empty()
        ? std::string(TranslationConstants::kDefaultOpenAiModel)
        : trimAscii(settings.openaiModel);
    const int timeoutMs = std::clamp(
        settings.timeoutMs, TranslationConstants::kMinTimeoutMs, TranslationConstants::kMaxTimeoutMs);

    return cfg.mergePatch({
        {"translate", {
            {"provider", provider},
            {"freePrimary", freePrimary},
            {"targetLang", targetLang},
            {"secondaryTargetLang", secondaryLang},
            {"openaiBaseUrl", openaiBase},
            {"openaiApiKey", protectSecretForStorage(trimAscii(settings.openaiApiKey))},
            {"openaiModel", openaiModel},
            {"deeplBaseUrl", trimAscii(settings.deeplBaseUrl)},
            {"deeplApiKey", protectSecretForStorage(trimAscii(settings.deeplApiKey))},
            {"timeoutMs", timeoutMs}
        }}
    }, "/translate");
}

void TranslationService::clearTokenCache() {
    std::lock_guard<std::mutex> lock(m_edgeTokenMutex);
    m_cachedBingCreds = BingSessionCreds{};
}

bool TranslationService::parseBingTranslatorCredentials(
    std::string_view htmlBody,
    std::string& outIg,
    std::string& outIid,
    std::string& outKey,
    std::string& outToken) {
    outIg.clear();
    outIid = "translator.5023";
    outKey.clear();
    outToken.clear();

    // 1. 提取 IG:"..."
    const size_t igPos = htmlBody.find("IG:\"");
    if (igPos != std::string_view::npos) {
        const size_t valStart = igPos + 4;
        const size_t valEnd = htmlBody.find('"', valStart);
        if (valEnd != std::string_view::npos && valEnd > valStart && (valEnd - valStart) <= 64) {
            outIg = std::string(htmlBody.substr(valStart, valEnd - valStart));
        }
    }

    // 2. 提取 data-iid="..."（可选，有安全默认值）
    const size_t iidPos = htmlBody.find("data-iid=\"");
    if (iidPos != std::string_view::npos) {
        const size_t valStart = iidPos + 10;
        const size_t valEnd = htmlBody.find('"', valStart);
        if (valEnd != std::string_view::npos && valEnd > valStart && (valEnd - valStart) <= 64) {
            outIid = std::string(htmlBody.substr(valStart, valEnd - valStart));
        }
    }

    // 3. 提取 params_AbusePreventionHelper = [key,"token",ttl]
    const size_t helperPos = htmlBody.find("params_AbusePreventionHelper");
    if (helperPos == std::string_view::npos) return false;
    const size_t lb = htmlBody.find('[', helperPos);
    if (lb == std::string_view::npos) return false;
    const size_t comma = htmlBody.find(',', lb + 1);
    if (comma == std::string_view::npos || comma <= lb + 1) return false;
    const std::string keyCand = trimAscii(htmlBody.substr(lb + 1, comma - (lb + 1)));
    if (keyCand.empty() || keyCand.size() > 32 ||
        !std::all_of(keyCand.begin(), keyCand.end(), [](unsigned char c) { return std::isdigit(c) != 0; })) {
        return false;
    }
    const size_t q1 = htmlBody.find('"', comma + 1);
    if (q1 == std::string_view::npos) return false;
    const size_t q2 = htmlBody.find('"', q1 + 1);
    if (q2 == std::string_view::npos || q2 <= q1 + 1 || (q2 - q1 - 1) > 256) return false;

    outKey = keyCand;
    outToken = std::string(htmlBody.substr(q1 + 1, q2 - q1 - 1));
    return !outIg.empty() && !outKey.empty() && !outToken.empty();
}

bool TranslationService::acquireBingCredentials(
    int timeoutMs,
    const std::atomic<bool>* cancelFlag,
    BingSessionCreds& outCreds,
    std::string& outErrorCode,
    std::string& outErrorDetail) {
    const int64_t now = unixNowSeconds();
    {
        std::lock_guard<std::mutex> lock(m_edgeTokenMutex);
        if (!m_cachedBingCreds.token.empty() && now < m_cachedBingCreds.expireUnix) {
            ++m_cachedBingCreds.reqSeq;
            outCreds = m_cachedBingCreds;
            return true;
        }
    }

    // 优先使用大陆免代理直连的 cn.bing.com，失败自动回退全球节点 www.bing.com
    static constexpr const char* kBingHosts[] = {"cn.bing.com", "www.bing.com"};
    for (const char* host : kBingHosts) {
        const ParsedHttpUrl pageUrl = crackUrl(std::string("https://") + host + "/translator");
        const WinHttpResult http = executeHttpRequest(
            pageUrl, L"GET", L"Accept: text/html\r\n", "", timeoutMs, cancelFlag);
        if (http.cancelled) {
            outErrorCode = "cancelled";
            return false;
        }
        if (!http.transportOk) {
            outErrorCode = "network";
            outErrorDetail = std::string(host) + "_winhttp=" + std::to_string(http.win32Error);
            continue;
        }
        if (http.statusCode < 200 || http.statusCode >= 300) {
            outErrorCode = (http.statusCode == 429) ? "rate_limited" : "http_error";
            outErrorDetail = std::string(host) + "_http_" + std::to_string(http.statusCode);
            continue;
        }

        BingSessionCreds fresh{};
        fresh.host = host;
        if (!parseBingTranslatorCredentials(http.body, fresh.ig, fresh.iid, fresh.key, fresh.token)) {
            outErrorCode = "invalid_response";
            outErrorDetail = std::string(host) + "_creds_parse_failed";
            continue;
        }
        fresh.expireUnix = unixNowSeconds() + 1500; // 25 分钟安全有效期（官方 TTL 60 分钟）
        fresh.reqSeq = 1;
        {
            std::lock_guard<std::mutex> lock(m_edgeTokenMutex);
            m_cachedBingCreds = fresh;
        }
        outCreds = std::move(fresh);
        return true;
    }
    return false;
}

TranslationBatchResult TranslationService::translateBatch(
    const TranslationBatchRequest& request,
    const std::atomic<bool>* cancelFlag) {
    return translateBatchWithSettings(request, loadSettings(), cancelFlag);
}

TranslationBatchResult TranslationService::translateBatchWithSettings(
    const TranslationBatchRequest& request,
    const TranslationSettings& settings,
    const std::atomic<bool>* cancelFlag) {
    const auto startedAt = std::chrono::steady_clock::now();
    auto stampElapsed = [&](TranslationBatchResult r) {
        r.elapsedMs = static_cast<uint32_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - startedAt).count());
        return r;
    };

    if (request.texts.empty()) {
        TranslationBatchResult emptyRes{};
        emptyRes.errorCode = "empty_input";
        return stampElapsed(std::move(emptyRes));
    }

    const std::string configuredTarget = !request.targetLangOverride.empty()
        ? request.targetLangOverride
        : settings.targetLang;
    const std::string effectiveTarget = resolveEffectiveTargetLanguage(
        request.texts, configuredTarget, settings.secondaryTargetLang, request.uiLanguageCode);

    const TranslationProviderKind kind = parseProviderKind(settings.provider);

    // 透明预处理加速管线：
    // 1. 已属目标语种的混合标签（如 Postman "$batch-UDTs批量新增表"）免发网络直接保留原文；
    // 2. 会话级 LRU 缓存命中（峰值 <128KB）直接 0ms 复用；
    // 3. 批内重复 UI 词条（如多处 "reply" / "Key" / "是"）先去重再发网，返回后按索引扇出回填。
    bool batchHasHan = false;
    for (const auto& t : request.texts) {
        const std::wstring ws = WinUtils::utf8ToWstring(t);
        for (wchar_t ch : ws) {
            const uint32_t cp = static_cast<uint32_t>(ch);
            if ((cp >= 0x4E00 && cp <= 0x9FFF) || (cp >= 0x3400 && cp <= 0x4DBF)) {
                batchHasHan = true;
                break;
            }
        }
        if (batchHasHan) break;
    }

    std::string cacheScope = settings.provider + "|" + effectiveTarget;
    if (kind == TranslationProviderKind::AutoFree) {
        cacheScope += "|" + settings.freePrimary;
    } else if (kind == TranslationProviderKind::OpenAiCompatible) {
        cacheScope += "|" + settings.openaiBaseUrl + "|" + settings.openaiModel;
    } else if (kind == TranslationProviderKind::DeepL) {
        cacheScope += "|" + settings.deeplBaseUrl;
    }

    std::vector<std::string> mergedOut(request.texts.size());
    std::vector<std::string> uniqueToTranslate;
    std::unordered_map<std::string, size_t> uniqueIndex;
    std::vector<std::pair<size_t, size_t>> slotToUnique;
    uniqueToTranslate.reserve(request.texts.size());
    slotToUnique.reserve(request.texts.size());

    auto& sessionCache = SessionTranslationCache::instance();
    for (size_t i = 0; i < request.texts.size(); ++i) {
        const std::string& src = request.texts[i];
        if (!request.bypassCache) {
            if (textAlreadyInTargetLanguage(src, effectiveTarget, batchHasHan)) {
                mergedOut[i] = src;
                continue;
            }
            std::string cachedVal;
            if (sessionCache.lookup(cacheScope + "|" + src, cachedVal)) {
                mergedOut[i] = std::move(cachedVal);
                continue;
            }
        }
        auto it = uniqueIndex.find(src);
        if (it == uniqueIndex.end()) {
            const size_t uIdx = uniqueToTranslate.size();
            uniqueIndex.emplace(src, uIdx);
            uniqueToTranslate.push_back(src);
            slotToUnique.emplace_back(i, uIdx);
        } else {
            slotToUnique.emplace_back(i, it->second);
        }
    }

    if (uniqueToTranslate.empty()) {
        TranslationBatchResult fastRes{};
        fastRes.success = true;
        fastRes.providerUsed = (kind == TranslationProviderKind::OpenAiCompatible)
            ? "openai_cache"
            : ((kind == TranslationProviderKind::DeepL) ? "deepl_cache" : "session_cache");
        fastRes.effectiveTargetLang = effectiveTarget;
        fastRes.translations = std::move(mergedOut);
        return stampElapsed(std::move(fastRes));
    }

    auto finalizeWithMerge = [&](TranslationBatchResult subRes) -> TranslationBatchResult {
        if (!subRes.success || subRes.translations.size() != uniqueToTranslate.size()) {
            return stampElapsed(std::move(subRes));
        }
        if (!request.bypassCache) {
            for (size_t u = 0; u < uniqueToTranslate.size(); ++u) {
                if (!subRes.translations[u].empty()) {
                    sessionCache.put(cacheScope + "|" + uniqueToTranslate[u], subRes.translations[u]);
                }
            }
        }
        for (const auto& [origIdx, uIdx] : slotToUnique) {
            mergedOut[origIdx] = subRes.translations[uIdx];
        }
        subRes.translations = std::move(mergedOut);
        return stampElapsed(std::move(subRes));
    };

    if (kind == TranslationProviderKind::OpenAiCompatible) {
        OpenAiCompatibleProvider provider;
        return finalizeWithMerge(provider.translate(uniqueToTranslate, effectiveTarget, settings, cancelFlag));
    }
    if (kind == TranslationProviderKind::DeepL) {
        DeepLProvider provider;
        return finalizeWithMerge(provider.translate(uniqueToTranslate, effectiveTarget, settings, cancelFlag));
    }

    // AutoFree 双源故障转移：遵循 settings.freePrimary 决定主/备顺序，任一失败无缝回退另一通道
    EdgeFreeProvider edgeProvider(
        [this](int timeoutMs, const std::atomic<bool>* cancel, BingCredsSnapshot& snap,
               std::string& errCode, std::string& errDetail) {
            BingSessionCreds creds{};
            if (!acquireBingCredentials(timeoutMs, cancel, creds, errCode, errDetail)) {
                return false;
            }
            snap.host = std::move(creds.host);
            snap.ig = std::move(creds.ig);
            snap.iid = std::move(creds.iid);
            snap.key = std::move(creds.key);
            snap.token = std::move(creds.token);
            snap.reqSeq = creds.reqSeq;
            return true;
        },
        [this]() { clearTokenCache(); });
    GoogleFreeProvider googleProvider;

    const int64_t nowSec = unixNowSeconds();
    const bool edgeInCooldown = (nowSec < m_edgeCooldownUntilUnix.load(std::memory_order_acquire));
    const bool googleFirst =
        (settings.freePrimary == TranslationConstants::kFreePrimaryGoogle) || edgeInCooldown;
    ITranslationProvider& primaryProvider = googleFirst
        ? static_cast<ITranslationProvider&>(googleProvider)
        : static_cast<ITranslationProvider&>(edgeProvider);
    ITranslationProvider& fallbackProvider = googleFirst
        ? static_cast<ITranslationProvider&>(edgeProvider)
        : static_cast<ITranslationProvider&>(googleProvider);

    TranslationBatchResult primaryRes = primaryProvider.translate(
        uniqueToTranslate, effectiveTarget, settings, cancelFlag);
    if (primaryRes.success || primaryRes.errorCode == "cancelled") {
        return finalizeWithMerge(std::move(primaryRes));
    }

    LOG_WARN("TranslationService: primary free provider {} failed ({}:{}), falling back to {}",
             primaryProvider.id(), primaryRes.errorCode, primaryRes.errorDetail, fallbackProvider.id());

    TranslationBatchResult fallbackRes = fallbackProvider.translate(
        uniqueToTranslate, effectiveTarget, settings, cancelFlag);
    if (fallbackRes.success || fallbackRes.errorCode == "cancelled") {
        // 若主通道 edge_free 触发风控/超时/拆行失败并成功由 google_free 兜底，开启 5 分钟智能冷却避免连环空等
        if (fallbackRes.success && !googleFirst && std::string_view(primaryProvider.id()) == "edge_free") {
            m_edgeCooldownUntilUnix.store(unixNowSeconds() + 300, std::memory_order_release);
        }
        return finalizeWithMerge(std::move(fallbackRes));
    }

    fallbackRes.errorDetail = std::string(primaryProvider.id()) + "=" + primaryRes.errorCode + "(" +
                              primaryRes.errorDetail + "); " + fallbackProvider.id() + "=" +
                              fallbackRes.errorCode + "(" + fallbackRes.errorDetail + ")";
    return stampElapsed(std::move(fallbackRes));
}

// ─────────────────────────────────────────────────────────────────────────────
// 纯静态方法与协议解析器
// ─────────────────────────────────────────────────────────────────────────────

TranslationProviderKind TranslationService::parseProviderKind(std::string_view provider) noexcept {
    if (provider == TranslationConstants::kProviderOpenAiCompatible) {
        return TranslationProviderKind::OpenAiCompatible;
    }
    if (provider == TranslationConstants::kProviderDeepL) {
        return TranslationProviderKind::DeepL;
    }
    return TranslationProviderKind::AutoFree;
}

std::string TranslationService::canonicalizeLanguageCode(std::string_view raw) {
    const std::string low = toLowerAscii(trimAscii(raw));
    if (low.empty() || low == "auto") return "auto";
    if (low == "zh" || low == "zh-cn" || low == "zh-hans" || low == "zh_cn") return "zh-CN";
    if (low == "zh-tw" || low == "zh-hant" || low == "zh_tw" || low == "zh-hk") return "zh-TW";
    if (low == "en" || low == "en-us" || low == "en-gb") return "en";
    if (low == "ja" || low == "ja-jp" || low == "jp") return "ja";
    if (low == "ko" || low == "ko-kr") return "ko";
    if (low == "ru" || low == "ru-ru") return "ru";
    if (low == "pt" || low == "pt-br" || low == "pt_br") return "pt-BR";
    if (low == "es" || low == "es-es") return "es";
    if (low == "fr" || low == "fr-fr") return "fr";
    if (low == "de" || low == "de-de") return "de";
    return trimAscii(raw);
}

std::string TranslationService::resolveEffectiveTargetLanguage(
    const std::vector<std::string>& texts,
    std::string_view configuredTargetLang,
    std::string_view secondaryTargetLang,
    std::string_view uiLanguageCode) {
    const std::string cfgTarget = canonicalizeLanguageCode(configuredTargetLang);
    std::string primary = (cfgTarget == "auto")
        ? canonicalizeLanguageCode(uiLanguageCode)
        : cfgTarget;
    if (primary.empty() || primary == "auto") primary = "zh-CN";

    std::string secondary = canonicalizeLanguageCode(secondaryTargetLang);
    if (secondary.empty() || secondary == "auto") {
        if (cfgTarget == "auto") {
            secondary = (primary == "en") ? "zh-CN" : "en";
        } else {
            return primary;
        }
    }
    if (secondary == primary) {
        return primary;
    }

    const ScriptStats stats = analyzeScriptStats(texts);
    if (stats.totalLetters == 0) {
        return primary;
    }

    const float hanRatio = static_cast<float>(stats.hanCount) / static_cast<float>(stats.totalLetters);
    const float kanaRatio = static_cast<float>(stats.kanaCount) / static_cast<float>(stats.totalLetters);
    const float hangulRatio = static_cast<float>(stats.hangulCount) / static_cast<float>(stats.totalLetters);
    const float cyrillicRatio = static_cast<float>(stats.cyrillicCount) / static_cast<float>(stats.totalLetters);
    const float latinRatio = static_cast<float>(stats.latinCount) / static_cast<float>(stats.totalLetters);

    if (primary == "zh-CN" || primary == "zh-TW") {
        // 含明显假名时不误判为中文（日语含大量汉字 + 假名）
        const bool isJapanese = (kanaRatio >= 0.08f);
        if (!isJapanese && hanRatio >= TranslationConstants::kAutoCjkThresholdRatio) {
            return secondary;
        }
        return primary;
    }
    if (primary == "ja") {
        return (kanaRatio >= 0.12f) ? secondary : primary;
    }
    if (primary == "ko") {
        return (hangulRatio >= 0.25f) ? secondary : primary;
    }
    if (primary == "ru") {
        return (cyrillicRatio >= 0.25f) ? secondary : primary;
    }
    if (primary == "en") {
        return (latinRatio >= 0.70f && hanRatio < 0.05f && kanaRatio < 0.05f && hangulRatio < 0.05f)
            ? secondary
            : primary;
    }
    return primary;
}

bool TranslationService::isLoopbackHttpUrl(std::string_view url) noexcept {
    const std::string trimmed = trimAscii(url);
    std::string_view rest;
    if (startsWithNoCase(trimmed, "http://")) {
        rest = std::string_view(trimmed).substr(7);
    } else if (startsWithNoCase(trimmed, "https://")) {
        rest = std::string_view(trimmed).substr(8);
    } else {
        return false;
    }
    const size_t hostEnd = rest.find_first_of("/?:");
    std::string host = toLowerAscii(hostEnd == std::string_view::npos ? rest : rest.substr(0, hostEnd));
    if (host.empty()) return false;
    if (host == "localhost" || host == "127.0.0.1" || host == "[::1]") return true;
    // 支持 IPv6 括号地址 "[::1]:11434"
    if (startsWithNoCase(rest, "[::1]")) return true;
    return false;
}

bool TranslationService::isKeylessPublicOpenAiUrl(std::string_view url) noexcept {
    const std::string trimmed = trimAscii(url);
    if (!startsWithNoCase(trimmed, "https://")) {
        return false;
    }
    const std::string_view rest = std::string_view(trimmed).substr(8);
    const size_t hostEnd = rest.find_first_of("/?:");
    const std::string host = toLowerAscii(hostEnd == std::string_view::npos ? rest : rest.substr(0, hostEnd));
    return host == TranslationConstants::kIndexTranslateHost;
}

OpenAiBatchPolicy TranslationService::resolveOpenAiBatchPolicy(
    std::string_view fullUrl,
    std::string_view model,
    int configuredTimeoutMs) noexcept {
    OpenAiBatchPolicy policy{};
    const int clampedConfigured = std::clamp(
        configuredTimeoutMs, TranslationConstants::kMinTimeoutMs, TranslationConstants::kMaxTimeoutMs);

    if (isKeylessPublicOpenAiUrl(fullUrl)) {
        const std::string lowModel = toLowerAscii(trimAscii(model));
        int minTimeout = 20000;
        if (lowModel.find("35b") != std::string::npos) {
            policy.maxCharsPerBatch = 1600;
            policy.maxItemsPerBatch = 8;
            minTimeout = 25000;
        } else if (lowModel.find("2b") != std::string::npos) {
            policy.maxCharsPerBatch = 3200;
            policy.maxItemsPerBatch = 18;
            minTimeout = 15000;
        } else {
            // 9B 均衡推荐档及 Nailong / Homura 等衍生模型
            policy.maxCharsPerBatch = 2400;
            policy.maxItemsPerBatch = 12;
            minTimeout = 20000;
        }
        policy.maxConcurrency = 2;
        policy.effectiveTimeoutMs = std::clamp(
            (std::max)(clampedConfigured, minTimeout),
            TranslationConstants::kMinTimeoutMs,
            TranslationConstants::kMaxTimeoutMs);
        return policy;
    }

    if (isLoopbackHttpUrl(fullUrl)) {
        // 本机 Ollama / LM Studio / vLLM 多为单卡串行推理队列，严禁高并发争抢显存引发排队超时
        policy.maxCharsPerBatch = 2800;
        policy.maxItemsPerBatch = 12;
        policy.maxConcurrency = 1;
        policy.effectiveTimeoutMs = std::clamp(
            (std::max)(clampedConfigured, 25000),
            TranslationConstants::kMinTimeoutMs,
            TranslationConstants::kMaxTimeoutMs);
        return policy;
    }

    policy.maxCharsPerBatch = 4500;
    policy.maxItemsPerBatch = 20;
    policy.maxConcurrency = 2;
    policy.effectiveTimeoutMs = clampedConfigured;
    return policy;
}

bool TranslationService::normalizeOpenAiChatUrl(std::string_view baseUrl, std::string& outFullUrl) {
    outFullUrl.clear();
    std::string url = trimAscii(baseUrl);
    if (url.empty()) return false;
    if (!startsWithNoCase(url, "https://") && !startsWithNoCase(url, "http://")) {
        return false;
    }
    while (!url.empty() && url.back() == '/') {
        url.pop_back();
    }
    if (endsWithNoCase(url, "/chat/completions")) {
        outFullUrl = url;
    } else if (endsWithNoCase(url, "/v1")) {
        outFullUrl = url + "/chat/completions";
    } else {
        outFullUrl = url + "/v1/chat/completions";
    }
    return crackUrl(outFullUrl).valid;
}

bool TranslationService::normalizeDeepLUrl(
    std::string_view customBaseUrl,
    std::string_view apiKey,
    std::string& outFullUrl) {
    outFullUrl.clear();
    std::string url = trimAscii(customBaseUrl);
    if (url.empty()) {
        const std::string key = trimAscii(apiKey);
        const bool isFree = endsWithNoCase(key, ":fx");
        outFullUrl = isFree
            ? "https://api-free.deepl.com/v2/translate"
            : "https://api.deepl.com/v2/translate";
        return true;
    }
    if (!startsWithNoCase(url, "https://") && !startsWithNoCase(url, "http://")) {
        return false;
    }
    while (!url.empty() && url.back() == '/') {
        url.pop_back();
    }
    if (endsWithNoCase(url, "/v2/translate")) {
        outFullUrl = url;
    } else if (endsWithNoCase(url, "/v2")) {
        outFullUrl = url + "/translate";
    } else {
        outFullUrl = url + "/v2/translate";
    }
    return crackUrl(outFullUrl).valid;
}

std::string TranslationService::toEdgeLangCode(std::string_view canonicalLang) {
    const std::string c = canonicalizeLanguageCode(canonicalLang);
    if (c == "zh-CN") return "zh-Hans";
    if (c == "zh-TW") return "zh-Hant";
    if (c == "pt-BR") return "pt";
    if (c == "auto") return "zh-Hans";
    return c;
}

std::string TranslationService::toGoogleLangCode(std::string_view canonicalLang) {
    const std::string c = canonicalizeLanguageCode(canonicalLang);
    if (c == "pt-BR") return "pt";
    if (c == "auto") return "zh-CN";
    return c;
}

std::string TranslationService::toDeepLLangCode(std::string_view canonicalLang) {
    const std::string c = canonicalizeLanguageCode(canonicalLang);
    if (c == "zh-CN") return "ZH-HANS";
    if (c == "zh-TW") return "ZH-HANT";
    if (c == "en") return "EN-US";
    if (c == "ja") return "JA";
    if (c == "ko") return "KO";
    if (c == "ru") return "RU";
    if (c == "pt-BR") return "PT-BR";
    if (c == "es") return "ES";
    if (c == "fr") return "FR";
    if (c == "de") return "DE";
    return "ZH-HANS";
}

std::string TranslationService::toHumanLanguageName(std::string_view canonicalLang) {
    const std::string c = canonicalizeLanguageCode(canonicalLang);
    if (c == "zh-CN") return "Simplified Chinese";
    if (c == "zh-TW") return "Traditional Chinese";
    if (c == "en") return "English";
    if (c == "ja") return "Japanese";
    if (c == "ko") return "Korean";
    if (c == "ru") return "Russian";
    if (c == "pt-BR") return "Brazilian Portuguese";
    if (c == "es") return "Spanish";
    if (c == "fr") return "French";
    if (c == "de") return "German";
    return c;
}

bool TranslationService::parseEdgeTranslateResponse(
    std::string_view responseBody,
    size_t expectedCount,
    std::vector<std::string>& outTranslations,
    std::string& outDetectedLang) {
    outTranslations.clear();
    outDetectedLang.clear();
    try {
        const auto doc = json::parse(responseBody.begin(), responseBody.end());
        if (!doc.is_array() || doc.size() != expectedCount) return false;
        outTranslations.reserve(expectedCount);
        for (const auto& entry : doc) {
            if (!entry.is_object()) return false;
            if (outDetectedLang.empty() && entry.contains("detectedLanguage") &&
                entry["detectedLanguage"].is_object()) {
                outDetectedLang = entry["detectedLanguage"].value("language", "");
            }
            if (!entry.contains("translations") || !entry["translations"].is_array() ||
                entry["translations"].empty()) {
                return false;
            }
            outTranslations.push_back(entry["translations"][0].value("text", ""));
        }
        return outTranslations.size() == expectedCount;
    } catch (...) {
        outTranslations.clear();
        return false;
    }
}

bool TranslationService::parseGoogleTranslateResponse(
    std::string_view responseBody,
    std::string& outMergedText,
    std::string& outDetectedLang) {
    outMergedText.clear();
    outDetectedLang.clear();
    try {
        const auto doc = json::parse(responseBody.begin(), responseBody.end());
        if (!doc.is_array() || doc.empty() || !doc[0].is_array()) return false;
        for (const auto& seg : doc[0]) {
            if (seg.is_array() && !seg.empty() && seg[0].is_string()) {
                outMergedText += seg[0].get<std::string>();
            }
        }
        if (doc.size() >= 3 && doc[2].is_string()) {
            outDetectedLang = doc[2].get<std::string>();
        }
        return !outMergedText.empty();
    } catch (...) {
        outMergedText.clear();
        return false;
    }
}

bool TranslationService::parseOpenAiBatchResponse(
    std::string_view responseBody,
    size_t expectedCount,
    std::vector<std::string>& outTranslations) {
    outTranslations.clear();
    if (expectedCount == 0) return true;

    auto extractSafeId = [](const json& item) noexcept -> int {
        if (!item.is_object() || !item.contains("id")) return -1;
        const auto& idVal = item["id"];
        if (idVal.is_number_integer()) {
            const int64_t v = idVal.get<int64_t>();
            if (v >= 0 && v <= static_cast<int64_t>((std::numeric_limits<int>::max)())) {
                return static_cast<int>(v);
            }
            return -1;
        }
        if (idVal.is_string()) {
            const std::string s = trimAscii(idVal.get<std::string>());
            if (s.empty() || s.size() > 9) return -1;
            int acc = 0;
            for (unsigned char c : s) {
                if (c < '0' || c > '9') return -1;
                acc = acc * 10 + static_cast<int>(c - '0');
            }
            return acc;
        }
        return -1;
    };

    auto extractSafeText = [](const json& item) -> std::string {
        if (!item.is_object()) return {};
        static constexpr const char* kKeys[] = {"text", "translation", "translated"};
        for (const char* key : kKeys) {
            if (!item.contains(key)) continue;
            const auto& val = item[key];
            if (val.is_string()) return val.get<std::string>();
            if (val.is_number() || val.is_boolean()) return val.dump();
        }
        return {};
    };

    auto trySinglePlainTextFallback = [&](const std::string& rawContent) -> bool {
        if (expectedCount != 1) return false;
        std::string plain = stripReasoningThinkBlocks(rawContent);
        if (plain.rfind("```", 0) == 0) {
            const size_t firstNewline = plain.find('\n');
            if (firstNewline != std::string::npos) {
                plain = plain.substr(firstNewline + 1);
                const size_t closeFence = plain.rfind("```");
                if (closeFence != std::string::npos) {
                    plain = plain.substr(0, closeFence);
                }
                plain = trimAscii(plain);
            }
        }
        if (plain.empty()) return false;
        // 若明显是截断或畸形的 JSON 对象/数组片段，绝不冒充自然语言译文落画布
        if (plain.front() == '{' || plain.front() == '[' ||
            plain.find("\"translations\"") != std::string::npos) {
            return false;
        }
        outTranslations = {std::move(plain)};
        return true;
    };

    std::string content;
    try {
        const auto envelope = json::parse(responseBody.begin(), responseBody.end());
        if (!envelope.is_object() || !envelope.contains("choices") ||
            !envelope["choices"].is_array() || envelope["choices"].empty()) {
            return false;
        }
        const auto& firstChoice = envelope["choices"][0];
        if (!firstChoice.is_object() || !firstChoice.contains("message") ||
            !firstChoice["message"].is_object()) {
            return false;
        }
        const auto& msg = firstChoice["message"];
        if (msg.contains("content") && msg["content"].is_string()) {
            content = msg["content"].get<std::string>();
        }
    } catch (...) {
        outTranslations.clear();
        return false;
    }

    if (content.empty()) return false;

    try {
        const std::string jsonStr = extractJsonPayloadFromModelContent(content);
        if (jsonStr.empty()) {
            return trySinglePlainTextFallback(content);
        }

        const auto parsed = json::parse(jsonStr);
        const json* arr = nullptr;
        if (parsed.is_object() && parsed.contains("translations") && parsed["translations"].is_array()) {
            arr = &parsed["translations"];
        } else if (parsed.is_array()) {
            arr = &parsed;
        } else if (parsed.is_object() && expectedCount == 1) {
            const std::string directText = extractSafeText(parsed);
            if (!trimAscii(directText).empty()) {
                outTranslations = {directText};
                return true;
            }
            return false;
        }
        if (!arr || arr->empty()) {
            return trySinglePlainTextFallback(content);
        }

        outTranslations.assign(expectedCount, {});
        std::vector<bool> filled(expectedCount, false);
        size_t sequentialCursor = 0;

        for (const auto& item : *arr) {
            if (item.is_object()) {
                const int id = extractSafeId(item);
                const std::string text = extractSafeText(item);
                if (trimAscii(text).empty()) continue;
                if (id >= 0 && static_cast<size_t>(id) < expectedCount) {
                    outTranslations[static_cast<size_t>(id)] = text;
                    filled[static_cast<size_t>(id)] = true;
                } else {
                    while (sequentialCursor < expectedCount && filled[sequentialCursor]) {
                        ++sequentialCursor;
                    }
                    if (sequentialCursor < expectedCount) {
                        outTranslations[sequentialCursor] = text;
                        filled[sequentialCursor] = true;
                        ++sequentialCursor;
                    }
                }
            } else if (item.is_string()) {
                const std::string text = item.get<std::string>();
                if (trimAscii(text).empty()) continue;
                while (sequentialCursor < expectedCount && filled[sequentialCursor]) {
                    ++sequentialCursor;
                }
                if (sequentialCursor < expectedCount) {
                    outTranslations[sequentialCursor] = text;
                    filled[sequentialCursor] = true;
                    ++sequentialCursor;
                }
            }
        }

        const size_t filledCount = static_cast<size_t>(std::count(filled.begin(), filled.end(), true));
        // 严格要求整批 100% 完整回填，杜绝模型输出截断导致半页被静默擦白；不完整时交由调用方自动二分补译
        if (filledCount == expectedCount) {
            return true;
        }
        outTranslations.clear();
        return false;
    } catch (...) {
        outTranslations.clear();
        return trySinglePlainTextFallback(content);
    }
}

bool TranslationService::parseDeepLTranslateResponse(
    std::string_view responseBody,
    size_t expectedCount,
    std::vector<std::string>& outTranslations,
    std::string& outDetectedLang) {
    outTranslations.clear();
    outDetectedLang.clear();
    try {
        const auto doc = json::parse(responseBody.begin(), responseBody.end());
        if (!doc.is_object() || !doc.contains("translations") || !doc["translations"].is_array()) {
            return false;
        }
        const auto& arr = doc["translations"];
        if (arr.size() != expectedCount) return false;
        outTranslations.reserve(expectedCount);
        for (const auto& item : arr) {
            if (!item.is_object()) return false;
            if (outDetectedLang.empty()) {
                outDetectedLang = item.value("detected_source_language", "");
            }
            outTranslations.push_back(item.value("text", ""));
        }
        return outTranslations.size() == expectedCount;
    } catch (...) {
        outTranslations.clear();
        return false;
    }
}

std::string TranslationService::protectSecretForStorage(std::string_view plainSecret) {
    if (plainSecret.empty()) return {};
    if (plainSecret.rfind(kDpapiPrefix, 0) == 0) {
        return std::string(plainSecret);
    }

    DATA_BLOB inBlob{};
    inBlob.pbData = reinterpret_cast<BYTE*>(const_cast<char*>(plainSecret.data()));
    inBlob.cbData = static_cast<DWORD>(plainSecret.size());
    DATA_BLOB outBlob{};
    if (!CryptProtectData(
            &inBlob,
            L"Tools3000.TranslateKey",
            nullptr,
            nullptr,
            nullptr,
            CRYPTPROTECT_UI_FORBIDDEN,
            &outBlob)) {
        return std::string(plainSecret);
    }
    std::string encoded = std::string(kDpapiPrefix) + base64Encode(outBlob.pbData, outBlob.cbData);
    LocalFree(outBlob.pbData);
    return encoded;
}

std::string TranslationService::unprotectSecretFromStorage(std::string_view storedSecret) {
    if (storedSecret.empty()) return {};
    if (storedSecret.rfind(kDpapiPrefix, 0) != 0) {
        return std::string(storedSecret);
    }
    std::vector<uint8_t> cipherBytes;
    if (!base64Decode(storedSecret.substr(std::strlen(kDpapiPrefix)), cipherBytes) || cipherBytes.empty()) {
        return {};
    }
    DATA_BLOB inBlob{};
    inBlob.pbData = cipherBytes.data();
    inBlob.cbData = static_cast<DWORD>(cipherBytes.size());
    DATA_BLOB outBlob{};
    if (!CryptUnprotectData(
            &inBlob,
            nullptr,
            nullptr,
            nullptr,
            nullptr,
            CRYPTPROTECT_UI_FORBIDDEN,
            &outBlob)) {
        return {};
    }
    std::string plain(reinterpret_cast<const char*>(outBlob.pbData), outBlob.cbData);
    SecureZeroMemory(outBlob.pbData, outBlob.cbData);
    LocalFree(outBlob.pbData);
    return plain;
}

std::string TranslationService::maskApiKeyForUi(std::string_view plainSecret) {
    const std::string trimmed = trimAscii(plainSecret);
    if (trimmed.empty()) return {};
    if (trimmed.size() <= 8) return "********";
    return trimmed.substr(0, 4) + "****" + trimmed.substr(trimmed.size() - 4);
}

} // namespace tools3000::core::translate
