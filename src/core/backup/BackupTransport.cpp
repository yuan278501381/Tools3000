// ─────────────────────────────────────────────────────────────────────────────
// BackupTransport.cpp — Folder (同步盘) / WebDAV / S3 (SigV4) 多态传输策略实现
// ─────────────────────────────────────────────────────────────────────────────

#include "core/backup/BackupTransport.h"
#include "core/backup/BackupCrypto.h"
#include "core/logger/Logger.h"
#include "core/utils/WinUtils.h"

#include <windows.h>
#include <winhttp.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <sstream>

namespace tools3000::core::backup {

IBackupTransport::~IBackupTransport() = default;

bool isSafeBackupFileName(std::string_view fileName) noexcept {
    if (fileName.empty() || fileName.size() > 160u) return false;
    if (fileName.find("..") != std::string_view::npos) return false;
    for (unsigned char ch : fileName) {
        if (ch == '/' || ch == '\\' || ch == ':' || ch == '*' || ch == '?' ||
            ch == '"' || ch == '<' || ch == '>' || ch == '|' || ch < 0x20u) {
            return false;
        }
    }
    return true;
}

namespace {

std::string trimStr(std::string_view sv) {
    size_t s = 0;
    while (s < sv.size() && std::isspace(static_cast<unsigned char>(sv[s]))) ++s;
    size_t e = sv.size();
    while (e > s && std::isspace(static_cast<unsigned char>(sv[e - 1u]))) --e;
    return std::string(sv.substr(s, e - s));
}

std::string toLowerAscii(std::string_view sv) {
    std::string out(sv);
    std::transform(out.begin(), out.end(), out.begin(), [](unsigned char ch) {
        return static_cast<char>(std::tolower(ch));
    });
    return out;
}

bool endsWithIgnoreCase(std::string_view text, std::string_view suffix) {
    if (text.size() < suffix.size()) return false;
    return toLowerAscii(text.substr(text.size() - suffix.size())) == toLowerAscii(suffix);
}

bool startsWithIgnoreCase(std::string_view text, std::string_view prefix) {
    if (text.size() < prefix.size()) return false;
    return toLowerAscii(text.substr(0, prefix.size())) == toLowerAscii(prefix);
}

bool isManagedBackupFileName(std::string_view name) {
    return isSafeBackupFileName(name) &&
           startsWithIgnoreCase(name, kBackupFilePrefix) &&
           (endsWithIgnoreCase(name, kBackupFileExtension) || endsWithIgnoreCase(name, ".json"));
}

bool isEncryptedBackupFileName(std::string_view name) {
    return endsWithIgnoreCase(name, kEncryptedBackupFileExtension) ||
           endsWithIgnoreCase(name, ".enc.json");
}

std::string xmlDecodeBasic(std::string_view s) {
    std::string out;
    out.reserve(s.size());
    for (size_t i = 0; i < s.size();) {
        if (s[i] == '&') {
            if (s.compare(i, 5, "&amp;") == 0) { out.push_back('&'); i += 5; continue; }
            if (s.compare(i, 4, "&lt;") == 0) { out.push_back('<'); i += 4; continue; }
            if (s.compare(i, 4, "&gt;") == 0) { out.push_back('>'); i += 4; continue; }
            if (s.compare(i, 6, "&quot;") == 0) { out.push_back('"'); i += 6; continue; }
            if (s.compare(i, 6, "&apos;") == 0) { out.push_back('\''); i += 6; continue; }
        }
        out.push_back(s[i++]);
    }
    return out;
}

std::string urlDecodeSimple(std::string_view s) {
    auto hexVal = [](char c) -> int {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'a' && c <= 'f') return 10 + (c - 'a');
        if (c >= 'A' && c <= 'F') return 10 + (c - 'A');
        return -1;
    };
    std::string out;
    out.reserve(s.size());
    for (size_t i = 0; i < s.size(); ++i) {
        if (s[i] == '%' && i + 2u < s.size()) {
            const int hi = hexVal(s[i + 1u]);
            const int lo = hexVal(s[i + 2u]);
            if (hi >= 0 && lo >= 0) {
                out.push_back(static_cast<char>((hi << 4) | lo));
                i += 2u;
                continue;
            }
        }
        out.push_back(s[i]);
    }
    return out;
}

/// 轻量不区分命名空间前缀的 XML 标签内容提取（如匹配 <D:href> / <d:href> / <href>）
std::vector<std::string> extractXmlBlocksByLocalTag(std::string_view xml, std::string_view localTagLower) {
    std::vector<std::string> blocks;
    const std::string lowXml = toLowerAscii(xml);
    size_t pos = 0;
    while (pos < lowXml.size()) {
        const size_t lt = lowXml.find('<', pos);
        if (lt == std::string::npos) break;
        const size_t gt = lowXml.find('>', lt + 1u);
        if (gt == std::string::npos) break;
        pos = gt + 1u;

        // 跳过结束标签、注释与声明
        if (lt + 1u >= gt || lowXml[lt + 1u] == '/' || lowXml[lt + 1u] == '!' || lowXml[lt + 1u] == '?') {
            continue;
        }
        // 提取完整标签名（到空格或 '/' 结束）
        size_t nameEnd = lt + 1u;
        while (nameEnd < gt && !std::isspace(static_cast<unsigned char>(lowXml[nameEnd])) && lowXml[nameEnd] != '/') {
            ++nameEnd;
        }
        const std::string_view fullTag(lowXml.data() + lt + 1u, nameEnd - (lt + 1u));
        const size_t colon = fullTag.rfind(':');
        const std::string_view local = (colon == std::string_view::npos) ? fullTag : fullTag.substr(colon + 1u);
        if (local != localTagLower) continue;

        // 自闭合标签无内容
        if (lowXml[gt - 1u] == '/') continue;

        const std::string closeToken = "</" + std::string(fullTag) + ">";
        const size_t closePos = lowXml.find(closeToken, gt + 1u);
        if (closePos == std::string::npos) break;

        blocks.emplace_back(xml.substr(gt + 1u, closePos - (gt + 1u)));
        pos = closePos + closeToken.size();
    }
    return blocks;
}

std::string extractFirstXmlValue(std::string_view xml, std::string_view localTagLower) {
    const auto blocks = extractXmlBlocksByLocalTag(xml, localTagLower);
    return blocks.empty() ? std::string{} : xmlDecodeBasic(trimStr(blocks.front()));
}

struct ParsedHttpUrl {
    bool isHttps = true;
    std::string host;
    INTERNET_PORT port = INTERNET_DEFAULT_HTTPS_PORT;
    std::string hostHeader; // 若非默认端口则包含 :port
    std::string path;       // 始终以 '/' 开头
};

bool parseHttpUrl(std::string_view rawUrl, ParsedHttpUrl& out) {
    out = {};
    const std::string trimmed = trimStr(rawUrl);
    if (trimmed.empty()) return false;

    const std::wstring wUrl = WinUtils::utf8ToWstring(trimmed);
    URL_COMPONENTSW uc{};
    uc.dwStructSize = sizeof(uc);
    wchar_t schemeBuf[16]{};
    wchar_t hostBuf[256]{};
    wchar_t pathBuf[2048]{};
    uc.lpszScheme = schemeBuf;
    uc.dwSchemeLength = static_cast<DWORD>(std::size(schemeBuf));
    uc.lpszHostName = hostBuf;
    uc.dwHostNameLength = static_cast<DWORD>(std::size(hostBuf));
    uc.lpszUrlPath = pathBuf;
    uc.dwUrlPathLength = static_cast<DWORD>(std::size(pathBuf));

    if (!WinHttpCrackUrl(wUrl.c_str(), static_cast<DWORD>(wUrl.size()), 0, &uc)) {
        return false;
    }
    if (uc.nScheme != INTERNET_SCHEME_HTTP && uc.nScheme != INTERNET_SCHEME_HTTPS) {
        return false;
    }
    out.isHttps = (uc.nScheme == INTERNET_SCHEME_HTTPS);
    out.host = WinUtils::wstringToUtf8(std::wstring(uc.lpszHostName, uc.dwHostNameLength));
    if (out.host.empty()) return false;
    out.port = uc.nPort;
    const bool isDefaultPort = (out.isHttps && out.port == INTERNET_DEFAULT_HTTPS_PORT) ||
                               (!out.isHttps && out.port == INTERNET_DEFAULT_HTTP_PORT);
    out.hostHeader = isDefaultPort ? out.host : (out.host + ":" + std::to_string(out.port));
    out.path = WinUtils::wstringToUtf8(std::wstring(uc.lpszUrlPath, uc.dwUrlPathLength));
    if (out.path.empty()) out.path = "/";
    return true;
}

struct WinHttpResponse {
    DWORD statusCode = 0;
    std::string body;
    std::string error;
};

WinHttpResponse executeWinHttpRequest(
    const ParsedHttpUrl& url,
    const std::wstring& method,
    const std::string& pathAndQuery,
    const std::vector<std::pair<std::string, std::string>>& headers,
    const std::string& requestBody) {
    WinHttpResponse resp{};

    HINTERNET hSession = WinHttpOpen(
        L"Tools3000/1.0 (ConfigBackup)",
        WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,
        WINHTTP_NO_PROXY_NAME,
        WINHTTP_NO_PROXY_BYPASS,
        0);
    if (!hSession) {
        resp.error = "winhttp_open_failed";
        return resp;
    }
    WinHttpSetTimeouts(
        hSession,
        static_cast<int>(kHttpConnectTimeoutMs),
        static_cast<int>(kHttpConnectTimeoutMs),
        static_cast<int>(kHttpRequestTimeoutMs),
        static_cast<int>(kHttpRequestTimeoutMs));

    const std::wstring hostW = WinUtils::utf8ToWstring(url.host);
    HINTERNET hConnect = WinHttpConnect(hSession, hostW.c_str(), url.port, 0);
    if (!hConnect) {
        WinHttpCloseHandle(hSession);
        resp.error = "winhttp_connect_failed";
        return resp;
    }

    const std::wstring targetW = WinUtils::utf8ToWstring(pathAndQuery.empty() ? "/" : pathAndQuery);
    const DWORD flags = url.isHttps ? WINHTTP_FLAG_SECURE : 0;
    HINTERNET hRequest = WinHttpOpenRequest(
        hConnect, method.c_str(), targetW.c_str(), nullptr,
        WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, flags);
    if (!hRequest) {
        WinHttpCloseHandle(hConnect);
        WinHttpCloseHandle(hSession);
        resp.error = "winhttp_request_open_failed";
        return resp;
    }

    std::wstring headerBlock;
    for (const auto& [k, v] : headers) {
        headerBlock += WinUtils::utf8ToWstring(k + ": " + v + "\r\n");
    }

    BOOL ok = WinHttpSendRequest(
        hRequest,
        headerBlock.empty() ? WINHTTP_NO_ADDITIONAL_HEADERS : headerBlock.c_str(),
        headerBlock.empty() ? 0 : static_cast<DWORD>(headerBlock.size()),
        requestBody.empty() ? WINHTTP_NO_REQUEST_DATA : const_cast<char*>(requestBody.data()),
        static_cast<DWORD>(requestBody.size()),
        static_cast<DWORD>(requestBody.size()),
        0);
    if (ok) {
        ok = WinHttpReceiveResponse(hRequest, nullptr);
    }
    if (!ok) {
        const DWORD err = GetLastError();
        WinHttpCloseHandle(hRequest);
        WinHttpCloseHandle(hConnect);
        WinHttpCloseHandle(hSession);
        resp.error = "winhttp_io_error_" + std::to_string(err);
        return resp;
    }

    DWORD statusSize = sizeof(resp.statusCode);
    WinHttpQueryHeaders(
        hRequest,
        WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
        WINHTTP_HEADER_NAME_BY_INDEX,
        &resp.statusCode,
        &statusSize,
        WINHTTP_NO_HEADER_INDEX);

    while (true) {
        DWORD available = 0;
        if (!WinHttpQueryDataAvailable(hRequest, &available) || available == 0) {
            break;
        }
        if (resp.body.size() + available > kMaxBackupPayloadBytes) {
            resp.error = "response_too_large";
            break;
        }
        const size_t oldSz = resp.body.size();
        resp.body.resize(oldSz + available);
        DWORD read = 0;
        if (!WinHttpReadData(hRequest, resp.body.data() + oldSz, available, &read)) {
            resp.error = "winhttp_read_failed";
            break;
        }
        resp.body.resize(oldSz + read);
    }

    WinHttpCloseHandle(hRequest);
    WinHttpCloseHandle(hConnect);
    WinHttpCloseHandle(hSession);
    return resp;
}

std::string fileTimeToIsoUtc(const std::filesystem::file_time_type& ft) {
    try {
        const auto sctp = std::chrono::time_point_cast<std::chrono::system_clock::duration>(
            ft - std::filesystem::file_time_type::clock::now() + std::chrono::system_clock::now());
        const std::time_t tt = std::chrono::system_clock::to_time_t(sctp);
        std::tm utc{};
        gmtime_s(&utc, &tt);
        char buf[32]{};
        std::snprintf(buf, sizeof(buf), "%04d-%02d-%02dT%02d:%02d:%02dZ",
                      utc.tm_year + 1900, utc.tm_mon + 1, utc.tm_mday,
                      utc.tm_hour, utc.tm_min, utc.tm_sec);
        return std::string(buf);
    } catch (...) {
        return {};
    }
}

void sortSnapshotsNewestFirst(std::vector<BackupSnapshotEntry>& entries) {
    std::sort(entries.begin(), entries.end(), [](const BackupSnapshotEntry& a, const BackupSnapshotEntry& b) {
        if (a.fileName != b.fileName) {
            return a.fileName > b.fileName; // tools3000-backup-YYYYMMDD-HHMMSS.t3kbak 天然按字典序即时间序
        }
        return a.modifiedAtIso > b.modifiedAtIso;
    });
}

// ═════════════════════════════════════════════════════════════════════════════
// 策略 1: 本地同步盘传输 (OneDrive / Google Drive / Dropbox / iCloud / 本地目录)
// ═════════════════════════════════════════════════════════════════════════════
class FolderBackupTransport final : public IBackupTransport {
public:
    explicit FolderBackupTransport(std::string folderPathUtf8)
        : m_dir(WinUtils::utf8ToWstring(trimStr(folderPathUtf8))) {}

    bool testConnection(std::string& outError, std::string& outDetail) override {
        if (m_dir.empty()) {
            outError = "missing_folder_path";
            return false;
        }
        std::error_code ec;
        std::filesystem::create_directories(m_dir, ec);
        if (ec || !std::filesystem::is_directory(m_dir, ec)) {
            outError = "folder_create_failed";
            return false;
        }
        const std::filesystem::path probe = m_dir / L".tools3000_write_probe.tmp";
        {
            std::ofstream ofs(probe, std::ios::binary | std::ios::trunc);
            if (!ofs) {
                outError = "folder_not_writable";
                return false;
            }
            ofs << "ok";
        }
        std::filesystem::remove(probe, ec);
        outDetail = WinUtils::wstringToUtf8(m_dir.wstring());
        return true;
    }

    bool uploadSnapshot(
        const std::string& fileName,
        const std::string& payloadUtf8,
        std::string& outError) override {
        if (!isSafeBackupFileName(fileName)) {
            outError = "invalid_filename";
            return false;
        }
        std::error_code ec;
        std::filesystem::create_directories(m_dir, ec);
        if (ec) {
            outError = "folder_create_failed";
            return false;
        }
        const std::filesystem::path dest = m_dir / WinUtils::utf8ToWstring(fileName);
        const std::filesystem::path tmp = dest.wstring() + L".tmp";
        {
            std::ofstream ofs(tmp, std::ios::binary | std::ios::trunc);
            if (!ofs) {
                outError = "file_write_failed";
                return false;
            }
            ofs.write(payloadUtf8.data(), static_cast<std::streamsize>(payloadUtf8.size()));
            if (!ofs.good()) {
                outError = "file_write_failed";
                return false;
            }
        }
        std::filesystem::rename(tmp, dest, ec);
        if (ec) {
            std::filesystem::remove(dest, ec);
            ec.clear();
            std::filesystem::rename(tmp, dest, ec);
            if (ec) {
                std::filesystem::remove(tmp, ec);
                outError = "file_rename_failed";
                return false;
            }
        }
        return true;
    }

    bool listSnapshots(std::vector<BackupSnapshotEntry>& outEntries, std::string& outError) override {
        outEntries.clear();
        std::error_code ec;
        if (!std::filesystem::exists(m_dir, ec)) {
            return true;
        }
        if (!std::filesystem::is_directory(m_dir, ec)) {
            outError = "folder_not_directory";
            return false;
        }
        for (const auto& entry : std::filesystem::directory_iterator(m_dir, ec)) {
            if (ec) break;
            if (!entry.is_regular_file(ec)) continue;
            const std::string name = WinUtils::wstringToUtf8(entry.path().filename().wstring());
            if (!isManagedBackupFileName(name)) continue;

            BackupSnapshotEntry item{};
            item.id = name;
            item.fileName = name;
            item.sizeBytes = entry.file_size(ec);
            item.modifiedAtIso = fileTimeToIsoUtc(entry.last_write_time(ec));

            // 优先通过规范后缀判断加密状态，并兼容扫描旧版未带 .enc 后缀的加密包尾部字段
            item.encrypted = isEncryptedBackupFileName(name);
            if (!item.encrypted) {
                std::ifstream ifs(entry.path(), std::ios::binary);
                if (ifs) {
                    const uint64_t fsz = item.sizeBytes;
                    if (fsz > 1024u) {
                        ifs.seekg(static_cast<std::streamoff>(fsz - 1024u), std::ios::beg);
                    }
                    char tail[1025]{};
                    ifs.read(tail, 1024);
                    const std::string_view sv(tail, static_cast<size_t>(ifs.gcount()));
                    item.encrypted = sv.find("\"encrypted\":true") != std::string_view::npos ||
                                     sv.find("\"encrypted\": true") != std::string_view::npos;
                }
            }
            outEntries.push_back(std::move(item));
        }
        sortSnapshotsNewestFirst(outEntries);
        return true;
    }

    bool downloadSnapshot(
        const std::string& snapshotId,
        std::string& outPayloadUtf8,
        std::string& outError) override {
        outPayloadUtf8.clear();
        if (!isSafeBackupFileName(snapshotId)) {
            outError = "invalid_snapshot_id";
            return false;
        }
        const std::filesystem::path src = m_dir / WinUtils::utf8ToWstring(snapshotId);
        std::error_code ec;
        if (!std::filesystem::is_regular_file(src, ec)) {
            outError = "snapshot_not_found";
            return false;
        }
        const auto sz = std::filesystem::file_size(src, ec);
        if (ec || sz == 0 || sz > kMaxBackupPayloadBytes) {
            outError = "snapshot_invalid_size";
            return false;
        }
        std::ifstream ifs(src, std::ios::binary);
        if (!ifs) {
            outError = "file_read_failed";
            return false;
        }
        outPayloadUtf8.assign(std::istreambuf_iterator<char>(ifs), std::istreambuf_iterator<char>());
        return !outPayloadUtf8.empty();
    }

    bool deleteSnapshot(const std::string& snapshotId, std::string& outError) override {
        if (!isSafeBackupFileName(snapshotId)) {
            outError = "invalid_snapshot_id";
            return false;
        }
        const std::filesystem::path src = m_dir / WinUtils::utf8ToWstring(snapshotId);
        std::error_code ec;
        std::filesystem::remove(src, ec);
        if (ec) {
            outError = "file_delete_failed";
            return false;
        }
        return true;
    }

private:
    std::filesystem::path m_dir;
};

// ═════════════════════════════════════════════════════════════════════════════
// 策略 2: 标准 WebDAV 协议传输 (坚果云 / Nextcloud / OwnCloud / NAS / Alist)
// ═════════════════════════════════════════════════════════════════════════════
class WebDavBackupTransport final : public IBackupTransport {
public:
    WebDavBackupTransport(ParsedHttpUrl baseUrl, std::string username, std::string password)
        : m_url(std::move(baseUrl)), m_user(std::move(username)), m_pass(std::move(password)) {
        if (m_url.path.empty()) m_url.path = "/";
        if (m_url.path.back() != '/') m_url.path.push_back('/');
    }

    bool testConnection(std::string& outError, std::string& outDetail) override {
        if (!ensureRemoteDirectory(outError)) {
            return false;
        }
        auto headers = buildBaseHeaders();
        headers.emplace_back("Depth", "0");
        const auto resp = executeWinHttpRequest(m_url, L"PROPFIND", m_url.path, headers, "");
        if (!resp.error.empty()) {
            outError = resp.error;
            return false;
        }
        if (resp.statusCode == 401 || resp.statusCode == 403) {
            outError = "webdav_auth_failed";
            return false;
        }
        if (resp.statusCode < 200 || resp.statusCode >= 300) {
            outError = "webdav_http_" + std::to_string(resp.statusCode);
            return false;
        }
        outDetail = "HTTP " + std::to_string(resp.statusCode);
        return true;
    }

    bool uploadSnapshot(
        const std::string& fileName,
        const std::string& payloadUtf8,
        std::string& outError) override {
        if (!isSafeBackupFileName(fileName)) {
            outError = "invalid_filename";
            return false;
        }
        if (!ensureRemoteDirectory(outError)) {
            return false;
        }
        auto headers = buildBaseHeaders();
        headers.emplace_back("Content-Type", "application/json; charset=utf-8");
        const std::string targetPath = m_url.path + BackupCrypto::uriEncode(fileName, true);
        const auto resp = executeWinHttpRequest(m_url, L"PUT", targetPath, headers, payloadUtf8);
        if (!resp.error.empty()) {
            outError = resp.error;
            return false;
        }
        if (resp.statusCode == 401 || resp.statusCode == 403) {
            outError = "webdav_auth_failed";
            return false;
        }
        if (resp.statusCode < 200 || resp.statusCode >= 300) {
            outError = "webdav_put_http_" + std::to_string(resp.statusCode);
            return false;
        }
        return true;
    }

    bool listSnapshots(std::vector<BackupSnapshotEntry>& outEntries, std::string& outError) override {
        outEntries.clear();
        auto headers = buildBaseHeaders();
        headers.emplace_back("Depth", "1");
        headers.emplace_back("Content-Type", "application/xml; charset=utf-8");
        static constexpr const char* kPropfindBody =
            "<?xml version=\"1.0\" encoding=\"utf-8\"?>"
            "<D:propfind xmlns:D=\"DAV:\"><D:prop><D:getcontentlength/><D:getlastmodified/></D:prop></D:propfind>";

        const auto resp = executeWinHttpRequest(m_url, L"PROPFIND", m_url.path, headers, kPropfindBody);
        if (!resp.error.empty()) {
            outError = resp.error;
            return false;
        }
        if (resp.statusCode == 404) {
            return true; // 目录尚未创建时视为 0 份快照
        }
        if (resp.statusCode == 401 || resp.statusCode == 403) {
            outError = "webdav_auth_failed";
            return false;
        }
        if (resp.statusCode < 200 || resp.statusCode >= 300) {
            outError = "webdav_propfind_http_" + std::to_string(resp.statusCode);
            return false;
        }

        const auto responses = extractXmlBlocksByLocalTag(resp.body, "response");
        for (const auto& block : responses) {
            std::string href = extractFirstXmlValue(block, "href");
            if (href.empty()) continue;
            while (!href.empty() && href.back() == '/') href.pop_back();
            const size_t slash = href.rfind('/');
            const std::string leaf = urlDecodeSimple(slash == std::string::npos ? href : href.substr(slash + 1u));
            if (!isManagedBackupFileName(leaf)) continue;

            BackupSnapshotEntry item{};
            item.id = leaf;
            item.fileName = leaf;
            item.encrypted = isEncryptedBackupFileName(leaf);
            const std::string lenStr = extractFirstXmlValue(block, "getcontentlength");
            try {
                if (!lenStr.empty()) item.sizeBytes = std::stoull(lenStr);
            } catch (...) {
                item.sizeBytes = 0;
            }
            item.modifiedAtIso = extractFirstXmlValue(block, "getlastmodified");
            outEntries.push_back(std::move(item));
        }
        sortSnapshotsNewestFirst(outEntries);
        return true;
    }

    bool downloadSnapshot(
        const std::string& snapshotId,
        std::string& outPayloadUtf8,
        std::string& outError) override {
        outPayloadUtf8.clear();
        if (!isSafeBackupFileName(snapshotId)) {
            outError = "invalid_snapshot_id";
            return false;
        }
        const auto headers = buildBaseHeaders();
        const std::string targetPath = m_url.path + BackupCrypto::uriEncode(snapshotId, true);
        const auto resp = executeWinHttpRequest(m_url, L"GET", targetPath, headers, "");
        if (!resp.error.empty()) {
            outError = resp.error;
            return false;
        }
        if (resp.statusCode == 404) {
            outError = "snapshot_not_found";
            return false;
        }
        if (resp.statusCode < 200 || resp.statusCode >= 300) {
            outError = "webdav_get_http_" + std::to_string(resp.statusCode);
            return false;
        }
        outPayloadUtf8 = resp.body;
        return !outPayloadUtf8.empty();
    }

    bool deleteSnapshot(const std::string& snapshotId, std::string& outError) override {
        if (!isSafeBackupFileName(snapshotId)) {
            outError = "invalid_snapshot_id";
            return false;
        }
        const auto headers = buildBaseHeaders();
        const std::string targetPath = m_url.path + BackupCrypto::uriEncode(snapshotId, true);
        const auto resp = executeWinHttpRequest(m_url, L"DELETE", targetPath, headers, "");
        if (!resp.error.empty()) {
            outError = resp.error;
            return false;
        }
        if (resp.statusCode == 404 || (resp.statusCode >= 200 && resp.statusCode < 300)) {
            return true;
        }
        outError = "webdav_delete_http_" + std::to_string(resp.statusCode);
        return false;
    }

private:
    std::vector<std::pair<std::string, std::string>> buildBaseHeaders() const {
        std::vector<std::pair<std::string, std::string>> headers;
        if (!m_user.empty() || !m_pass.empty()) {
            const std::string cred = m_user + ":" + m_pass;
            headers.emplace_back(
                "Authorization",
                "Basic " + BackupCrypto::base64Encode(
                    reinterpret_cast<const uint8_t*>(cred.data()), cred.size()));
        }
        return headers;
    }

    bool ensureRemoteDirectory(std::string& outError) const {
        if (m_url.path == "/") return true;
        const auto headers = buildBaseHeaders();
        const auto resp = executeWinHttpRequest(m_url, L"MKCOL", m_url.path, headers, "");
        if (!resp.error.empty()) {
            outError = resp.error;
            return false;
        }
        // 201 Created / 405 Method Not Allowed (已存在) / 200 OK / 301 / 409 逐级目录容错
        if (resp.statusCode == 401 || resp.statusCode == 403) {
            outError = "webdav_auth_failed";
            return false;
        }
        return true;
    }

    ParsedHttpUrl m_url;
    std::string m_user;
    std::string m_pass;
};

// ═════════════════════════════════════════════════════════════════════════════
// 策略 3: 标准 S3 兼容协议传输 (AWS Signature Version 4 · R2/OSS/MinIO/AWS)
// ═════════════════════════════════════════════════════════════════════════════
class S3BackupTransport final : public IBackupTransport {
public:
    S3BackupTransport(
        ParsedHttpUrl endpointUrl,
        std::string region,
        std::string bucket,
        std::string prefix,
        std::string accessKeyId,
        std::string secretKey,
        bool forcePathStyle)
        : m_url(std::move(endpointUrl)),
          m_region(region.empty() ? "auto" : std::move(region)),
          m_bucket(trimStr(bucket)),
          m_accessKey(std::move(accessKeyId)),
          m_secretKey(std::move(secretKey)),
          m_forcePathStyle(forcePathStyle) {
        m_prefix = trimStr(prefix);
        while (!m_prefix.empty() && m_prefix.front() == '/') m_prefix.erase(m_prefix.begin());
        if (!m_prefix.empty() && m_prefix.back() != '/') m_prefix.push_back('/');

        if (!m_forcePathStyle && !m_bucket.empty()) {
            // Virtual-Host 模式：将 bucket 拼到 host 前缀
            m_url.host = m_bucket + "." + m_url.host;
            const bool isDef = (m_url.isHttps && m_url.port == INTERNET_DEFAULT_HTTPS_PORT) ||
                               (!m_url.isHttps && m_url.port == INTERNET_DEFAULT_HTTP_PORT);
            m_url.hostHeader = isDef ? m_url.host : (m_url.host + ":" + std::to_string(m_url.port));
        }
    }

    bool testConnection(std::string& outError, std::string& outDetail) override {
        std::vector<BackupSnapshotEntry> probeList;
        if (!listSnapshots(probeList, outError)) {
            return false;
        }
        outDetail = "S3 OK (" + std::to_string(probeList.size()) + " snapshots)";
        return true;
    }

    bool uploadSnapshot(
        const std::string& fileName,
        const std::string& payloadUtf8,
        std::string& outError) override {
        if (!isSafeBackupFileName(fileName)) {
            outError = "invalid_filename";
            return false;
        }
        const std::string objectKey = m_prefix + fileName;
        const std::string canonicalUri = buildObjectUri(objectKey);
        const std::string contentType = "application/json; charset=utf-8";
        const auto resp = sendSignedRequest(L"PUT", "PUT", canonicalUri, "", payloadUtf8, contentType);
        if (!resp.error.empty()) {
            outError = resp.error;
            return false;
        }
        if (resp.statusCode == 401 || resp.statusCode == 403) {
            outError = "s3_auth_failed";
            return false;
        }
        if (resp.statusCode < 200 || resp.statusCode >= 300) {
            outError = "s3_put_http_" + std::to_string(resp.statusCode);
            return false;
        }
        return true;
    }

    bool listSnapshots(std::vector<BackupSnapshotEntry>& outEntries, std::string& outError) override {
        outEntries.clear();
        const std::string canonicalUri = buildBucketUri();
        // SigV4 要求查询参数严格按 Key 字典序排列：list-type 在 prefix 之前
        std::string canonicalQuery = "list-type=2";
        if (!m_prefix.empty()) {
            canonicalQuery += "&prefix=" + BackupCrypto::uriEncode(m_prefix, true);
        }
        const auto resp = sendSignedRequest(L"GET", "GET", canonicalUri, canonicalQuery, "", "");
        if (!resp.error.empty()) {
            outError = resp.error;
            return false;
        }
        if (resp.statusCode == 401 || resp.statusCode == 403) {
            outError = "s3_auth_failed";
            return false;
        }
        if (resp.statusCode == 404) {
            outError = "s3_bucket_not_found";
            return false;
        }
        if (resp.statusCode < 200 || resp.statusCode >= 300) {
            outError = "s3_list_http_" + std::to_string(resp.statusCode);
            return false;
        }

        const auto contents = extractXmlBlocksByLocalTag(resp.body, "contents");
        for (const auto& block : contents) {
            const std::string key = extractFirstXmlValue(block, "key");
            if (key.empty()) continue;
            const size_t slash = key.rfind('/');
            const std::string leaf = (slash == std::string::npos) ? key : key.substr(slash + 1u);
            if (!isManagedBackupFileName(leaf)) continue;

            BackupSnapshotEntry item{};
            item.id = leaf;
            item.fileName = leaf;
            item.encrypted = isEncryptedBackupFileName(leaf);
            const std::string szStr = extractFirstXmlValue(block, "size");
            try {
                if (!szStr.empty()) item.sizeBytes = std::stoull(szStr);
            } catch (...) {
                item.sizeBytes = 0;
            }
            item.modifiedAtIso = extractFirstXmlValue(block, "lastmodified");
            outEntries.push_back(std::move(item));
        }
        sortSnapshotsNewestFirst(outEntries);
        return true;
    }

    bool downloadSnapshot(
        const std::string& snapshotId,
        std::string& outPayloadUtf8,
        std::string& outError) override {
        outPayloadUtf8.clear();
        if (!isSafeBackupFileName(snapshotId)) {
            outError = "invalid_snapshot_id";
            return false;
        }
        const std::string canonicalUri = buildObjectUri(m_prefix + snapshotId);
        const auto resp = sendSignedRequest(L"GET", "GET", canonicalUri, "", "", "");
        if (!resp.error.empty()) {
            outError = resp.error;
            return false;
        }
        if (resp.statusCode == 404) {
            outError = "snapshot_not_found";
            return false;
        }
        if (resp.statusCode == 401 || resp.statusCode == 403) {
            outError = "s3_auth_failed";
            return false;
        }
        if (resp.statusCode < 200 || resp.statusCode >= 300) {
            outError = "s3_get_http_" + std::to_string(resp.statusCode);
            return false;
        }
        outPayloadUtf8 = resp.body;
        return !outPayloadUtf8.empty();
    }

    bool deleteSnapshot(const std::string& snapshotId, std::string& outError) override {
        if (!isSafeBackupFileName(snapshotId)) {
            outError = "invalid_snapshot_id";
            return false;
        }
        const std::string canonicalUri = buildObjectUri(m_prefix + snapshotId);
        const auto resp = sendSignedRequest(L"DELETE", "DELETE", canonicalUri, "", "", "");
        if (!resp.error.empty()) {
            outError = resp.error;
            return false;
        }
        if (resp.statusCode == 404 || (resp.statusCode >= 200 && resp.statusCode < 300)) {
            return true;
        }
        outError = "s3_delete_http_" + std::to_string(resp.statusCode);
        return false;
    }

private:
    std::string buildBucketUri() const {
        if (m_forcePathStyle) {
            return "/" + BackupCrypto::uriEncode(m_bucket, true);
        }
        return "/";
    }

    std::string buildObjectUri(const std::string& objectKey) const {
        const std::string encodedKey = BackupCrypto::uriEncode(objectKey, false);
        if (m_forcePathStyle) {
            return "/" + BackupCrypto::uriEncode(m_bucket, true) + "/" + encodedKey;
        }
        return "/" + encodedKey;
    }

    WinHttpResponse sendSignedRequest(
        const std::wstring& methodW,
        const std::string& methodAscii,
        const std::string& canonicalUri,
        const std::string& canonicalQuery,
        const std::string& body,
        const std::string& contentType) const {
        S3SigV4Params sigParams{};
        sigParams.method = methodAscii;
        sigParams.host = m_url.hostHeader;
        sigParams.canonicalUri = canonicalUri;
        sigParams.canonicalQuery = canonicalQuery;
        sigParams.region = m_region;
        sigParams.service = "s3";
        sigParams.accessKeyId = m_accessKey;
        sigParams.secretAccessKey = m_secretKey;
        sigParams.payloadSha256Hex = BackupCrypto::sha256Hex(body);
        sigParams.contentType = contentType;

        const auto sig = BackupCrypto::buildS3SigV4Headers(sigParams);
        std::vector<std::pair<std::string, std::string>> headers;
        headers.emplace_back("x-amz-date", sig.amzDate);
        headers.emplace_back("x-amz-content-sha256", sig.payloadSha256);
        headers.emplace_back("Authorization", sig.authorization);
        if (!contentType.empty()) {
            headers.emplace_back("Content-Type", contentType);
        }

        const std::string pathAndQuery = canonicalQuery.empty()
            ? canonicalUri
            : (canonicalUri + "?" + canonicalQuery);
        return executeWinHttpRequest(m_url, methodW, pathAndQuery, headers, body);
    }

    ParsedHttpUrl m_url;
    std::string m_region;
    std::string m_bucket;
    std::string m_prefix;
    std::string m_accessKey;
    std::string m_secretKey;
    bool m_forcePathStyle = true;
};

}  // namespace

std::unique_ptr<IBackupTransport> createBackupTransport(
    const ResolvedBackupConfig& config,
    std::string& outError) {
    outError.clear();
    switch (config.settings.provider) {
        case BackupProviderType::Folder: {
            const std::string path = trimStr(config.settings.folderPath);
            if (path.empty()) {
                outError = "missing_folder_path";
                return nullptr;
            }
            return std::make_unique<FolderBackupTransport>(path);
        }
        case BackupProviderType::WebDav: {
            ParsedHttpUrl url{};
            if (!parseHttpUrl(config.settings.webdavUrl, url)) {
                outError = "invalid_webdav_url";
                return nullptr;
            }
            return std::make_unique<WebDavBackupTransport>(
                std::move(url), config.settings.webdavUsername, config.webdavPassword);
        }
        case BackupProviderType::S3: {
            ParsedHttpUrl url{};
            if (!parseHttpUrl(config.settings.s3Endpoint, url)) {
                outError = "invalid_s3_endpoint";
                return nullptr;
            }
            if (trimStr(config.settings.s3Bucket).empty()) {
                outError = "missing_s3_bucket";
                return nullptr;
            }
            if (trimStr(config.settings.s3AccessKeyId).empty() || config.s3SecretKey.empty()) {
                outError = "missing_s3_credentials";
                return nullptr;
            }
            return std::make_unique<S3BackupTransport>(
                std::move(url),
                config.settings.s3Region,
                config.settings.s3Bucket,
                config.settings.s3Prefix,
                config.settings.s3AccessKeyId,
                config.s3SecretKey,
                config.settings.s3ForcePathStyle);
        }
        case BackupProviderType::None:
        default:
            outError = "provider_not_configured";
            return nullptr;
    }
}

}  // namespace tools3000::core::backup
