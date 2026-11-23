#include "core/diagnostics/FeedbackDiagnosticService.h"
#include "core/config/ConfigManager.h"
#include "core/plugin/PluginManager.h"
#include "core/utils/DpiUtils.h"
#include "core/utils/WinUtils.h"
#include "Tools3000Version.h"

#include <windows.h>
#include <bcrypt.h>
#include <wincrypt.h>
#include <psapi.h>
#include <shlobj.h>
#include <winhttp.h>
#include <zlib.h>

#include <algorithm>
#include <array>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <deque>
#include <fstream>
#include <iomanip>
#include <regex>
#include <sstream>

using json = nlohmann::json;

namespace tools3000::core {

namespace {

constexpr const char* kGitHubIssuesNewUrl = "https://github.com/yuan278501381/tools3000/issues/new";

std::string sha256Hex(const std::string& data) {
    BCRYPT_ALG_HANDLE hAlg = nullptr;
    BCRYPT_HASH_HANDLE hHash = nullptr;
    if (BCryptOpenAlgorithmProvider(&hAlg, BCRYPT_SHA256_ALGORITHM, nullptr, 0) != 0) return {};
    DWORD hashObjSize = 0, cbData = 0;
    BCryptGetProperty(hAlg, BCRYPT_OBJECT_LENGTH, reinterpret_cast<PUCHAR>(&hashObjSize), sizeof(DWORD), &cbData, 0);
    std::vector<BYTE> hashObj(hashObjSize);
    std::array<BYTE, 32> digest{};
    std::string hex;
    if (BCryptCreateHash(hAlg, &hHash, hashObj.data(), hashObjSize, nullptr, 0, 0) == 0) {
        if (BCryptHashData(hHash, reinterpret_cast<PUCHAR>(const_cast<char*>(data.data())),
                           static_cast<ULONG>(data.size()), 0) == 0 &&
            BCryptFinishHash(hHash, digest.data(), static_cast<ULONG>(digest.size()), 0) == 0) {
            static constexpr char kHex[] = "0123456789abcdef";
            hex.reserve(64);
            for (BYTE b : digest) {
                hex.push_back(kHex[(b >> 4) & 0x0F]);
                hex.push_back(kHex[b & 0x0F]);
            }
        }
        BCryptDestroyHash(hHash);
    }
    BCryptCloseAlgorithmProvider(hAlg, 0);
    return hex;
}

std::string urlEncodeComponent(const std::string& raw) {
    static constexpr char kHex[] = "0123456789ABCDEF";
    std::string out;
    out.reserve(raw.size() * 2);
    for (unsigned char c : raw) {
        if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') ||
            c == '-' || c == '_' || c == '.' || c == '~') {
            out.push_back(static_cast<char>(c));
        } else {
            out.push_back('%');
            out.push_back(kHex[(c >> 4) & 0x0F]);
            out.push_back(kHex[c & 0x0F]);
        }
    }
    return out;
}

std::string readTailLinesUtf8(const std::filesystem::path& filePath, size_t maxLines) {
    std::error_code ec;
    if (filePath.empty() || !std::filesystem::is_regular_file(filePath, ec)) {
        return {};
    }
    HANDLE h = CreateFileW(
        filePath.wstring().c_str(),
        GENERIC_READ,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        nullptr,
        OPEN_EXISTING,
        FILE_ATTRIBUTE_NORMAL,
        nullptr);
    if (h == INVALID_HANDLE_VALUE) return {};

    LARGE_INTEGER sz{};
    std::string raw;
    if (GetFileSizeEx(h, &sz) && sz.QuadPart > 0) {
        const uint64_t total = static_cast<uint64_t>(sz.QuadPart);
        const uint64_t window = (std::min)(total, static_cast<uint64_t>(160u * 1024u));
        LARGE_INTEGER pos{};
        pos.QuadPart = static_cast<LONGLONG>(total - window);
        if (SetFilePointerEx(h, pos, nullptr, FILE_BEGIN)) {
            raw.resize(static_cast<size_t>(window));
            DWORD got = 0;
            if (ReadFile(h, raw.data(), static_cast<DWORD>(window), &got, nullptr)) {
                raw.resize(got);
            } else {
                raw.clear();
            }
        }
    }
    CloseHandle(h);
    if (raw.empty()) return {};

    std::deque<std::string> lines;
    size_t start = 0;
    while (start < raw.size()) {
        size_t nl = raw.find('\n', start);
        if (nl == std::string::npos) nl = raw.size();
        std::string line = raw.substr(start, nl - start);
        if (!line.empty() && line.back() == '\r') line.pop_back();
        lines.push_back(std::move(line));
        if (lines.size() > maxLines) {
            lines.pop_front();
        }
        start = nl + 1;
    }

    std::ostringstream oss;
    for (size_t i = 0; i < lines.size(); ++i) {
        if (i > 0) oss << '\n';
        oss << lines[i];
    }
    return oss.str();
}

std::string queryWindowsBuildString() {
    using RtlGetVersionFn = LONG(WINAPI*)(PRTL_OSVERSIONINFOW);
    HMODULE ntdll = GetModuleHandleW(L"ntdll.dll");
    if (ntdll) {
        auto* fn = reinterpret_cast<RtlGetVersionFn>(GetProcAddress(ntdll, "RtlGetVersion"));
        if (fn) {
            RTL_OSVERSIONINFOW vi{};
            vi.dwOSVersionInfoSize = sizeof(vi);
            if (fn(&vi) == 0) {
                return "Windows " + std::to_string(vi.dwMajorVersion) + "." +
                       std::to_string(vi.dwMinorVersion) + " (Build " +
                       std::to_string(vi.dwBuildNumber) + ")";
            }
        }
    }
    return "Windows (Unknown Build)";
}

json collectSanitizedEnvironmentSnapshot() {
    json monitors = json::array();
    EnumDisplayMonitors(
        nullptr, nullptr,
        [](HMONITOR hMon, HDC, LPRECT, LPARAM lp) -> BOOL {
            auto* arr = reinterpret_cast<json*>(lp);
            MONITORINFO mi{};
            mi.cbSize = sizeof(mi);
            if (GetMonitorInfoW(hMon, &mi)) {
                const unsigned dpi = tools3000::core::dpi::effectiveDpiForMonitor(hMon);
                arr->push_back({
                    {"width", mi.rcMonitor.right - mi.rcMonitor.left},
                    {"height", mi.rcMonitor.bottom - mi.rcMonitor.top},
                    {"dpi", dpi},
                    {"scalePercent", static_cast<int>(std::lround(tools3000::core::dpi::scaleForDpi(dpi) * 100.0f))},
                    {"primary", (mi.dwFlags & MONITORINFOF_PRIMARY) != 0}
                });
            }
            return TRUE;
        },
        reinterpret_cast<LPARAM>(&monitors));

    PROCESS_MEMORY_COUNTERS_EX pmc{};
    pmc.cb = sizeof(pmc);
    uint64_t wsMb = 0;
    uint64_t privMb = 0;
    if (GetProcessMemoryInfo(GetCurrentProcess(), reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&pmc), sizeof(pmc))) {
        wsMb = pmc.WorkingSetSize / (1024ull * 1024ull);
        privMb = pmc.PrivateUsage / (1024ull * 1024ull);
    }

    json enabledPlugins = json::array();
    for (const auto& ps : PluginManager::instance().getPluginStatuses()) {
        if (ps.enabled || ps.active) {
            enabledPlugins.push_back(ps.id + (ps.active ? ":active" : ":enabled"));
        }
    }

#if defined(_M_ARM64)
    const char* arch = "ARM64";
#elif defined(_M_X64)
    const char* arch = "x64";
#else
    const char* arch = "x86";
#endif

    auto& cfg = ConfigManager::instance();
    return {
        {"appVersion", tools3000::version::String},
        {"arch", arch},
        {"portableMode", WinUtils::isPortableMode()},
        {"elevated", WinUtils::isCurrentProcessElevated()},
        {"os", queryWindowsBuildString()},
        {"language", cfg.get<std::string>("/general/language", "auto")},
        {"theme", cfg.get<std::string>("/general/theme", "system")},
        {"monitors", std::move(monitors)},
        {"enabledPlugins", std::move(enabledPlugins)},
        {"hostMemoryMb", {{"workingSet", wsMb}, {"private", privMb}}}
    };
}

// 轻量零依赖标准 ZIP 写入器 (STORE + zlib crc32)
bool writeSimpleStoreZip(const std::filesystem::path& zipPath,
                         const std::vector<std::pair<std::string, std::string>>& entries) {
    struct CdRec {
        std::string name;
        uint32_t crc = 0;
        uint32_t size = 0;
        uint32_t localOffset = 0;
    };

    std::string out;
    out.reserve(128 * 1024);
    auto appendLe16 = [&](uint16_t v) {
        out.push_back(static_cast<char>(v & 0xFFu));
        out.push_back(static_cast<char>((v >> 8) & 0xFFu));
    };
    auto appendLe32 = [&](uint32_t v) {
        out.push_back(static_cast<char>(v & 0xFFu));
        out.push_back(static_cast<char>((v >> 8) & 0xFFu));
        out.push_back(static_cast<char>((v >> 16) & 0xFFu));
        out.push_back(static_cast<char>((v >> 24) & 0xFFu));
    };

    std::vector<CdRec> cds;
    cds.reserve(entries.size());
    for (const auto& [name, content] : entries) {
        const uint32_t localOff = static_cast<uint32_t>(out.size());
        const uint32_t crc = static_cast<uint32_t>(
            crc32(0L, reinterpret_cast<const Bytef*>(content.data()), static_cast<uInt>(content.size())));
        const uint32_t sz = static_cast<uint32_t>(content.size());

        appendLe32(0x04034b50u);  // Local file header
        appendLe16(20);           // version needed
        appendLe16(0x0800u);      // UTF-8 filename flag
        appendLe16(0);            // STORE
        appendLe16(0);            // mod time
        appendLe16(0);            // mod date
        appendLe32(crc);
        appendLe32(sz);
        appendLe32(sz);
        appendLe16(static_cast<uint16_t>(name.size()));
        appendLe16(0);            // extra len
        out.append(name);
        out.append(content);
        cds.push_back({name, crc, sz, localOff});
    }

    const uint32_t cdOffset = static_cast<uint32_t>(out.size());
    for (const auto& r : cds) {
        appendLe32(0x02014b50u);  // Central directory header
        appendLe16(20);
        appendLe16(20);
        appendLe16(0x0800u);
        appendLe16(0);
        appendLe16(0);
        appendLe16(0);
        appendLe32(r.crc);
        appendLe32(r.size);
        appendLe32(r.size);
        appendLe16(static_cast<uint16_t>(r.name.size()));
        appendLe16(0);
        appendLe16(0);
        appendLe16(0);
        appendLe16(0);
        appendLe32(0);
        appendLe32(r.localOffset);
        out.append(r.name);
    }
    const uint32_t cdSize = static_cast<uint32_t>(out.size()) - cdOffset;

    appendLe32(0x06054b50u);      // EOCD
    appendLe16(0);
    appendLe16(0);
    appendLe16(static_cast<uint16_t>(cds.size()));
    appendLe16(static_cast<uint16_t>(cds.size()));
    appendLe32(cdSize);
    appendLe32(cdOffset);
    appendLe16(0);

    std::error_code ec;
    std::filesystem::create_directories(zipPath.parent_path(), ec);
    return WinUtils::atomicWriteFileWithFlush(zipPath.wstring(), out);
}

std::string decodeBase64Payload(std::string_view b64) {
    if (b64.empty()) return {};
    DWORD outLen = 0;
    if (!CryptStringToBinaryA(b64.data(), static_cast<DWORD>(b64.size()),
                              CRYPT_STRING_BASE64, nullptr, &outLen, nullptr, nullptr) || outLen == 0) {
        return {};
    }
    std::string out(outLen, '\0');
    if (!CryptStringToBinaryA(b64.data(), static_cast<DWORD>(b64.size()),
                              CRYPT_STRING_BASE64, reinterpret_cast<BYTE*>(out.data()), &outLen, nullptr, nullptr)) {
        return {};
    }
    out.resize(outLen);
    return out;
}

bool postJsonToRelayEndpoint(const std::string& endpointUrl,
                             const std::string& powNonce,
                             const std::string& powChallenge,
                             const std::string& jsonBody,
                             std::string& outResponseBody,
                             DWORD& outStatusCode) {
    outResponseBody.clear();
    outStatusCode = 0;
    const std::wstring wUrl = WinUtils::utf8ToWstring(endpointUrl);
    if (wUrl.rfind(L"https://", 0) != 0) return false;

    URL_COMPONENTSW uc{};
    uc.dwStructSize = sizeof(uc);
    wchar_t host[256]{};
    wchar_t path[1024]{};
    uc.lpszHostName = host;
    uc.dwHostNameLength = static_cast<DWORD>(std::size(host));
    uc.lpszUrlPath = path;
    uc.dwUrlPathLength = static_cast<DWORD>(std::size(path));
    if (!WinHttpCrackUrl(wUrl.c_str(), 0, 0, &uc)) return false;

    HINTERNET hSession = WinHttpOpen(L"Tools3000-Feedback/1.0",
                                     WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,
                                     WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!hSession) return false;
    WinHttpSetTimeouts(hSession, 4000, 4000, 6000, 8000);

    HINTERNET hConnect = WinHttpConnect(hSession, host, uc.nPort, 0);
    if (!hConnect) {
        WinHttpCloseHandle(hSession);
        return false;
    }

    HINTERNET hReq = WinHttpOpenRequest(
        hConnect, L"POST", path[0] ? path : L"/", nullptr,
        WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE);
    if (!hReq) {
        WinHttpCloseHandle(hConnect);
        WinHttpCloseHandle(hSession);
        return false;
    }

    const std::wstring headers =
        L"Content-Type: application/json; charset=utf-8\r\n"
        L"X-Tools3000-Pow-Challenge: " + WinUtils::utf8ToWstring(powChallenge) + L"\r\n"
        L"X-Tools3000-Pow-Nonce: " + WinUtils::utf8ToWstring(powNonce) + L"\r\n";

    bool ok = false;
    if (WinHttpSendRequest(hReq, headers.c_str(), static_cast<DWORD>(headers.size()),
                           const_cast<char*>(jsonBody.data()), static_cast<DWORD>(jsonBody.size()),
                           static_cast<DWORD>(jsonBody.size()), 0) &&
        WinHttpReceiveResponse(hReq, nullptr)) {
        DWORD statusSize = sizeof(outStatusCode);
        WinHttpQueryHeaders(hReq, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                            WINHTTP_HEADER_NAME_BY_INDEX, &outStatusCode, &statusSize, WINHTTP_NO_HEADER_INDEX);
        DWORD avail = 0;
        while (WinHttpQueryDataAvailable(hReq, &avail) && avail > 0) {
            std::string chunk(avail, '\0');
            DWORD read = 0;
            if (WinHttpReadData(hReq, chunk.data(), avail, &read) && read > 0) {
                chunk.resize(read);
                outResponseBody += chunk;
            }
            if (outResponseBody.size() > 64 * 1024) break;
        }
        ok = (outStatusCode >= 200 && outStatusCode < 300);
    }

    WinHttpCloseHandle(hReq);
    WinHttpCloseHandle(hConnect);
    WinHttpCloseHandle(hSession);
    return ok;
}

}  // namespace

RedactionReport FeedbackDiagnosticService::redactSensitiveText(const std::string& input) {
    RedactionReport report;
    report.redactedText = input;
    if (input.empty()) return report;

    auto applyRule = [&](const std::regex& re, const std::string& replacement) {
        std::string out;
        out.reserve(report.redactedText.size());
        auto begin = std::sregex_iterator(report.redactedText.begin(), report.redactedText.end(), re);
        auto end = std::sregex_iterator();
        size_t lastPos = 0;
        for (auto it = begin; it != end; ++it) {
            const auto& m = *it;
            out.append(report.redactedText, lastPos, static_cast<size_t>(m.position()) - lastPos);
            out.append(m.format(replacement));
            lastPos = static_cast<size_t>(m.position() + m.length());
            ++report.totalReplacements;
        }
        if (lastPos > 0) {
            out.append(report.redactedText, lastPos, std::string::npos);
            report.redactedText = std::move(out);
        }
    };

    try {
        // 1. Windows 用户目录路径 (支持中英及非 ASCII 用户名)
        static const std::regex kWinUserPath(
            R"(([A-Za-z]:[/\\]Users[/\\])([^/\\\r\n\t"':*?<>|]+))", std::regex::icase);
        applyRule(kWinUserPath, "$1<USER>");

        // 2. URL 内嵌凭据 (https://user:pass@host)
        static const std::regex kUrlCreds(
            R"((https?://)([^/\s:@]+):([^/\s@]+)@)", std::regex::icase);
        applyRule(kUrlCreds, "$1[REDACTED_CRED]@");

        // 3. JWT & Bearer Token
        static const std::regex kBearerToken(
            R"((Bearer\s+)[A-Za-z0-9\-._~+/]+=*)", std::regex::icase);
        applyRule(kBearerToken, "$1[REDACTED_TOKEN]");
        static const std::regex kJwtToken(
            R"(\beyJ[A-Za-z0-9_-]{8,}\.[A-Za-z0-9_-]{8,}\.[A-Za-z0-9_-]{8,}\b)");
        applyRule(kJwtToken, "[REDACTED_TOKEN]");

        // 4. 主流平台 API Key / Token (GitHub / OpenAI / Anthropic / AWS / Slack)
        static const std::regex kApiKeys(
            R"(\b(?:ghp_[A-Za-z0-9]{20,}|gho_[A-Za-z0-9]{20,}|github_pat_[A-Za-z0-9_]{20,}|sk-[A-Za-z0-9\-_]{20,}|AKIA[0-9A-Z]{16}|xox[baprs]-[A-Za-z0-9-]{16,})\b)");
        applyRule(kApiKeys, "[REDACTED_KEY]");

        // 5. 通用敏感键值对 (password / token / secret / api_key / authorization)
        static const std::regex kSecretKv(
            R"(((?:password|passwd|secret|api[_-]?key|access[_-]?token|refresh[_-]?token|authorization)\s*[:=]\s*["']?)([^"'\s,;}\r\n]{4,}))",
            std::regex::icase);
        applyRule(kSecretKv, "$1[REDACTED_SECRET]");

        // 6. 电子邮箱
        static const std::regex kEmail(
            R"(\b[A-Za-z0-9._%+\-]+@[A-Za-z0-9.\-]+\.[A-Za-z]{2,}\b)");
        applyRule(kEmail, "[REDACTED_EMAIL]");

        // 7. Windows 账户 SID (保留前缀，隐藏域/用户段)
        static const std::regex kWinSid(
            R"(\bS-1-5-21-\d+-\d+-\d+-\d+\b)");
        applyRule(kWinSid, "S-1-5-21-[REDACTED_SID]");

        // 8. 非回环 IPv4 地址 (放行 127.0.0.1 / 0.0.0.0 便于诊断本地绑定)
        static const std::regex kIpv4(
            R"(\b(?:(?:25[0-5]|2[0-4]\d|1\d\d|[1-9]?\d)\.){3}(?:25[0-5]|2[0-4]\d|1\d\d|[1-9]?\d)\b)");
        {
            std::string out;
            out.reserve(report.redactedText.size());
            auto begin = std::sregex_iterator(report.redactedText.begin(), report.redactedText.end(), kIpv4);
            auto end = std::sregex_iterator();
            size_t lastPos = 0;
            for (auto it = begin; it != end; ++it) {
                const auto& m = *it;
                const std::string ip = m.str();
                if (ip == "127.0.0.1" || ip == "0.0.0.0") continue;
                out.append(report.redactedText, lastPos, static_cast<size_t>(m.position()) - lastPos);
                out.append("[REDACTED_IP]");
                lastPos = static_cast<size_t>(m.position() + m.length());
                ++report.totalReplacements;
            }
            if (lastPos > 0) {
                out.append(report.redactedText, lastPos, std::string::npos);
                report.redactedText = std::move(out);
            }
        }

        // 9. 中国大陆 11 位手机号 (精准边界，避免误伤长数字串)
        static const std::regex kCnPhone(
            R"((^|[^0-9])(1[3-9]\d{9})($|[^0-9]))");
        applyRule(kCnPhone, "$1[REDACTED_PHONE]$3");

        // 10. 18 位中国大陆居民身份证号
        static const std::regex kCnIdCard(
            R"((^|[^0-9])([1-9]\d{5}(?:19|20)\d{2}(?:0[1-9]|1[0-2])(?:0[1-9]|[12]\d|3[01])\d{3}[0-9Xx])($|[^0-9]))");
        applyRule(kCnIdCard, "$1[REDACTED_ID]$3");
    } catch (...) {
        // 正则引擎极端异常下安全返回当前已脱敏结果
    }

    return report;
}

FeedbackBundlePreview FeedbackDiagnosticService::buildDiagnosticPreview(
    const FeedbackSubmissionInput& input,
    size_t maxLogLines) {
    FeedbackBundlePreview preview;
    preview.environment = collectSanitizedEnvironmentSnapshot();

    const std::string relayUrl = ConfigManager::instance().get<std::string>("/feedback/relayEndpoint", "");
    preview.relayConfigured = (relayUrl.rfind("https://", 0) == 0);
    preview.relayEndpoint = relayUrl;

    uint32_t redactions = 0;
    if (input.includeLogs) {
        const std::filesystem::path mainLogPath = WinUtils::getLogDirectory() / L"tools3000.log";
        const std::string rawMain = readTailLinesUtf8(mainLogPath, maxLogLines);
        if (!rawMain.empty()) {
            auto r = redactSensitiveText(rawMain);
            preview.redactedMainLog = std::move(r.redactedText);
            redactions += r.totalReplacements;
        }

        PWSTR programData = nullptr;
        if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_ProgramData, 0, nullptr, &programData)) && programData) {
            const std::filesystem::path svcLogPath =
                std::filesystem::path(programData) / L"Tools3000" / L"logs" / L"Tools3000_Service.log";
            CoTaskMemFree(programData);
            const std::string rawSvc = readTailLinesUtf8(svcLogPath, maxLogLines);
            if (!rawSvc.empty()) {
                auto r = redactSensitiveText(rawSvc);
                preview.redactedServiceLog = std::move(r.redactedText);
                redactions += r.totalReplacements;
            }
        }
    }

    auto descRed = redactSensitiveText(input.description);
    auto stepsRed = redactSensitiveText(input.stepsToReproduce);
    redactions += descRed.totalReplacements + stepsRed.totalReplacements;
    preview.totalRedactions = redactions;

    const std::string typeTag =
        (input.feedbackType == "feature") ? "Feature Request" :
        (input.feedbackType == "performance") ? "Performance / Memory" :
        (input.feedbackType == "other") ? "General Feedback" : "Bug Report";

    std::ostringstream md;
    md << "## [" << typeTag << "] " << (input.title.empty() ? "User Feedback" : input.title) << "\n\n";
    md << "### Description\n" << (descRed.redactedText.empty() ? "(No description provided)" : descRed.redactedText) << "\n\n";
    if (!stepsRed.redactedText.empty()) {
        md << "### Steps to Reproduce\n" << stepsRed.redactedText << "\n\n";
    }
    md << "### Environment Snapshot\n```json\n" << preview.environment.dump(2) << "\n```\n\n";
    if (input.includeLogs && !preview.redactedMainLog.empty()) {
        // Markdown 内保留末 40 行摘要，完整 200 行随 .zip / 中继传输
        const size_t tailKeep = 2400;
        std::string excerpt = preview.redactedMainLog.size() > tailKeep
            ? preview.redactedMainLog.substr(preview.redactedMainLog.size() - tailKeep)
            : preview.redactedMainLog;
        md << "### Redacted Log Excerpt\n```text\n" << excerpt << "\n```\n";
    }
    preview.markdownReport = md.str();

    // 预填 GitHub Issue URL（受 URL 长度上限控制，保持 <= 3500 字符安全打开）
    const std::string issueTitle = "[" + typeTag + "] " + (input.title.empty() ? "Feedback" : input.title);
    std::string bodyForUrl = preview.markdownReport;
    if (bodyForUrl.size() > 2600) {
        bodyForUrl.resize(2600);
        bodyForUrl += "\n\n...(truncated, please attach the exported Tools3000 diagnostic .zip bundle)";
    }
    const std::string labelParam =
        (input.feedbackType == "feature") ? "enhancement" :
        (input.feedbackType == "performance") ? "performance" : "bug";
    preview.githubIssueUrl = std::string(kGitHubIssuesNewUrl) +
        "?labels=" + urlEncodeComponent(labelParam) +
        "&title=" + urlEncodeComponent(issueTitle) +
        "&body=" + urlEncodeComponent(bodyForUrl);

    return preview;
}

bool FeedbackDiagnosticService::exportDiagnosticZipBundle(
    const FeedbackSubmissionInput& input,
    const std::filesystem::path& outputZipPath,
    std::string& outError,
    uint32_t* outTotalRedactions) {
    outError.clear();
    const auto preview = buildDiagnosticPreview(input, 200);
    if (outTotalRedactions) *outTotalRedactions = preview.totalRedactions;

    std::vector<std::pair<std::string, std::string>> entries;
    entries.emplace_back("report.md", preview.markdownReport);
    entries.emplace_back("environment.json", preview.environment.dump(2));
    if (input.includeLogs && !preview.redactedMainLog.empty()) {
        entries.emplace_back("tools3000_redacted.log", preview.redactedMainLog);
    }
    if (input.includeLogs && !preview.redactedServiceLog.empty()) {
        entries.emplace_back("service_redacted.log", preview.redactedServiceLog);
    }

    // 附带截图（Data URI 或本地文件，限制单图 8 MB）
    if (!input.screenshotDataUri.empty()) {
        const size_t comma = input.screenshotDataUri.find(',');
        if (comma != std::string::npos) {
            const std::string header = input.screenshotDataUri.substr(0, comma);
            const std::string decoded = decodeBase64Payload(
                std::string_view(input.screenshotDataUri).substr(comma + 1));
            if (!decoded.empty() && decoded.size() <= 8u * 1024u * 1024u) {
                const std::string ext = (header.find("jpeg") != std::string::npos || header.find("jpg") != std::string::npos)
                    ? ".jpg" : ".png";
                entries.emplace_back("screenshot" + ext, decoded);
            }
        }
    } else if (!input.screenshotPath.empty()) {
        const std::filesystem::path imgPath = WinUtils::utf8ToPath(input.screenshotPath);
        std::error_code ec;
        if (std::filesystem::is_regular_file(imgPath, ec)) {
            const auto sz = std::filesystem::file_size(imgPath, ec);
            if (!ec && sz > 0 && sz <= 8u * 1024u * 1024u) {
                std::ifstream ifs(imgPath, std::ios::binary);
                if (ifs) {
                    std::string imgBytes((std::istreambuf_iterator<char>(ifs)), std::istreambuf_iterator<char>());
                    std::string ext = WinUtils::toLower(WinUtils::wstringToUtf8(imgPath.extension().wstring()));
                    if (ext.empty()) ext = ".png";
                    entries.emplace_back("screenshot" + ext, std::move(imgBytes));
                }
            }
        }
    }

    if (!writeSimpleStoreZip(outputZipPath, entries)) {
        outError = "write_zip_failed";
        return false;
    }
    return true;
}

std::string FeedbackDiagnosticService::computeLightweightPowNonce(
    const std::string& challengePrefix,
    uint32_t leadingZeroHexDigits) {
    const uint32_t targetZeros = std::clamp(leadingZeroHexDigits, 1u, 6u);
    const std::string zeroPrefix(targetZeros, '0');
    for (uint64_t nonce = 0; nonce < 5'000'000ull; ++nonce) {
        const std::string candidate = std::to_string(nonce);
        const std::string digest = sha256Hex(challengePrefix + ":" + candidate);
        if (digest.size() >= targetZeros && digest.compare(0, targetZeros, zeroPrefix) == 0) {
            return candidate;
        }
    }
    return "0";
}

FeedbackSubmitOutcome FeedbackDiagnosticService::submitFeedback(const FeedbackSubmissionInput& input) {
    FeedbackSubmitOutcome outcome;
    const auto preview = buildDiagnosticPreview(input, 200);
    outcome.totalRedactions = preview.totalRedactions;
    outcome.markdownReport = preview.markdownReport;
    outcome.issueUrl = preview.githubIssueUrl;

    // 轨 1：若已配置 HTTPS Cloudflare Worker 中继，优先轻量 PoW 直交
    if (preview.relayConfigured) {
        const auto nowSec = static_cast<uint64_t>(std::time(nullptr));
        const std::string challenge = "t3k-" + std::to_string(nowSec);
        const std::string nonce = computeLightweightPowNonce(challenge, 4);

        const json payload = {
            {"type", input.feedbackType},
            {"title", input.title},
            {"description", redactSensitiveText(input.description).redactedText},
            {"stepsToReproduce", redactSensitiveText(input.stepsToReproduce).redactedText},
            {"contact", input.contact},
            {"environment", preview.environment},
            {"redactedMainLog", input.includeLogs ? preview.redactedMainLog : ""},
            {"redactedServiceLog", input.includeLogs ? preview.redactedServiceLog : ""},
            {"screenshotDataUri", input.screenshotDataUri.size() <= 3u * 1024u * 1024u ? input.screenshotDataUri : ""}
        };

        std::string respBody;
        DWORD statusCode = 0;
        if (postJsonToRelayEndpoint(preview.relayEndpoint, nonce, challenge, payload.dump(), respBody, statusCode)) {
            const json parsed = json::parse(respBody, nullptr, false);
            outcome.ok = true;
            outcome.mode = "relay_submitted";
            if (parsed.is_object()) {
                outcome.trackingId = parsed.value("trackingId", parsed.value("id", ""));
                if (parsed.contains("issueUrl") && parsed["issueUrl"].is_string()) {
                    outcome.issueUrl = parsed["issueUrl"].get<std::string>();
                }
            }
            return outcome;
        }
        outcome.errorMessage = "relay_unavailable_status_" + std::to_string(statusCode);
    }

    // 轨 2：无缝降级为本地脱敏 .zip 诊断包导出 + 预填 GitHub Issue 链接
    PWSTR downloads = nullptr;
    std::filesystem::path outDir;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_Downloads, 0, nullptr, &downloads)) && downloads) {
        outDir = std::filesystem::path(downloads);
        CoTaskMemFree(downloads);
    } else {
        outDir = WinUtils::getAppDataDirectory() / L"feedback";
    }

    const time_t nowT = std::time(nullptr);
    tm localTm{};
    localtime_s(&localTm, &nowT);
    wchar_t stamp[64]{};
    std::swprintf(stamp, std::size(stamp), L"%04d%02d%02d_%02d%02d%02d",
                  localTm.tm_year + 1900, localTm.tm_mon + 1, localTm.tm_mday,
                  localTm.tm_hour, localTm.tm_min, localTm.tm_sec);

    const std::filesystem::path zipPath = outDir / (L"Tools3000_Feedback_" + std::wstring(stamp) + L".zip");
    std::string zipErr;
    if (exportDiagnosticZipBundle(input, zipPath, zipErr)) {
        outcome.ok = true;
        outcome.mode = "fallback_local";
        outcome.exportedBundlePath = WinUtils::pathToUtf8(zipPath);
    } else {
        outcome.ok = false;
        outcome.mode = "fallback_local";
        outcome.errorMessage = zipErr;
    }
    return outcome;
}

std::string FeedbackDiagnosticService::getGitHubNewIssueBaseUrl() {
    return kGitHubIssuesNewUrl;
}

}  // namespace tools3000::core
