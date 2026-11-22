#pragma once
// ─────────────────────────────────────────────────────────────────────────────
// ExternalApiPolicy.h — Tools3000_Service 外部 CLI / MCP 纵深安全策略 (SSOT)
//
// 职责:
//   与 tools3000-cli.exe (Rust) 保持双层纵深防御一致性：
//   1. 强制外部客户端只读边界（仅放行 getDbStats 与只读搜索，严禁 shutdown/rebuild/history 变更）；
//   2. 服务端强制过滤高危敏感凭证目录/文件名/扩展名，即使非官方客户端直接连管道也无法绕过。
// ─────────────────────────────────────────────────────────────────────────────

#ifndef TOOLS3000_SERVICE_EXTERNALAPIPOLICY_H
#define TOOLS3000_SERVICE_EXTERNALAPIPOLICY_H

#include "PipeClientAuth.h"

#include <array>
#include <string>
#include <string_view>

namespace tools3000::service::external_api {

inline bool isExternalClientSource(std::string_view source) noexcept {
    return source == "external_cli" || source == "external_mcp";
}

inline bool isActionAllowedForExternal(std::string_view action) noexcept {
    return action == "getDbStats";
}

inline std::wstring normalizePathLower(std::wstring_view rawPath) {
    std::wstring norm;
    norm.reserve(rawPath.size());
    for (wchar_t ch : rawPath) {
        norm.push_back(tools3000::service::pipe_auth::foldAsciiOrWideLower(ch));
    }
    if (norm.size() >= 4 && norm.compare(0, 4, L"\\\\?\\") == 0) {
        norm.erase(0, 4);
    }
    while (norm.size() > 3 && norm.back() == L'\\') {
        norm.pop_back();
    }
    return norm;
}

inline bool isSensitiveCredentialPath(std::wstring_view rawPath) {
    const std::wstring norm = normalizePathLower(rawPath);
    if (norm.empty()) return true;

    static constexpr std::array<std::wstring_view, 8> kMultiSegmentSensitive = {
        L"microsoft\\credentials",
        L"microsoft\\vault",
        L"microsoft\\protect",
        L"microsoft\\crypto",
        L"google\\chrome\\user data",
        L"microsoft\\edge\\user data",
        L"mozilla\\firefox\\profiles",
        L"bravesoftware\\brave-browser\\user data",
    };
    for (const auto& seg : kMultiSegmentSensitive) {
        std::wstring mid;
        mid.reserve(seg.size() + 2);
        mid.push_back(L'\\');
        mid.append(seg);
        mid.push_back(L'\\');
        if (norm.find(mid) != std::wstring::npos) return true;
        std::wstring tail;
        tail.reserve(seg.size() + 1);
        tail.push_back(L'\\');
        tail.append(seg);
        if (norm.size() >= tail.size() &&
            norm.compare(norm.size() - tail.size(), tail.size(), tail) == 0) {
            return true;
        }
    }

    static constexpr std::array<std::wstring_view, 10> kSingleSegmentSensitive = {
        L".ssh",
        L".gnupg",
        L".aws",
        L".azure",
        L".kube",
        L".gcloud",
        L".terraform.d",
        L"1password",
        L"bitwarden",
        L"keepass",
    };

    std::wstring_view lastSeg;
    size_t pos = 0;
    while (pos < norm.size()) {
        const size_t sep = norm.find(L'\\', pos);
        const std::wstring_view part = (sep == std::wstring::npos)
            ? std::wstring_view(norm).substr(pos)
            : std::wstring_view(norm).substr(pos, sep - pos);
        if (!part.empty()) {
            lastSeg = part;
            for (const auto& s : kSingleSegmentSensitive) {
                if (part == s) return true;
            }
        }
        if (sep == std::wstring::npos) break;
        pos = sep + 1;
    }

    if (!lastSeg.empty()) {
        if (lastSeg == L".env" ||
            (lastSeg.size() > 5 && lastSeg.compare(0, 5, L".env.") == 0)) {
            return true;
        }
        static constexpr std::array<std::wstring_view, 22> kSensitiveFileNames = {
            L".git-credentials", L".npmrc", L".pypirc", L".netrc", L"_netrc",
            L"id_rsa", L"id_ed25519", L"id_ecdsa", L"id_dsa",
            L"authorized_keys", L"known_hosts",
            L"credentials.json", L"service-account.json",
            L"secrets.json", L"secrets.yaml", L"secrets.yml",
            L"login data", L"local state", L"cookies", L"web data",
            L"key4.db", L"logins.json",
        };
        for (const auto& f : kSensitiveFileNames) {
            if (lastSeg == f) return true;
        }
        if (lastSeg == L"ntuser.dat") return true;

        const size_t dot = lastSeg.rfind(L'.');
        if (dot != std::wstring_view::npos && dot + 1 < lastSeg.size()) {
            const std::wstring_view ext = lastSeg.substr(dot + 1);
            static constexpr std::array<std::wstring_view, 8> kSensitiveExts = {
                L"pem", L"key", L"p12", L"pfx", L"kdbx", L"jks", L"keystore", L"ppk",
            };
            for (const auto& se : kSensitiveExts) {
                if (ext == se) return true;
            }
        }
    }

    return false;
}

}  // namespace tools3000::service::external_api

#endif  // TOOLS3000_SERVICE_EXTERNALAPIPOLICY_H
