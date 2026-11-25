#pragma once
// ─────────────────────────────────────────────────────────────────────────────
// SensitivePathGuard.h — 浏览器敏感凭据/会话文件与系统机密目录零句柄触碰守卫 (SSOT)
//
// 设计目标:
//   防止 Tools3000_Service.exe 在 NTFS USN 实时属性刷新、后台目录水合、
//   搜索结果元数据查询或全文内容搜索期间对 Chrome / Edge / Brave / Opera /
//   Firefox / Vivaldi / Chromium 等浏览器已保存密码、Cookie、密钥库及 Windows
//   凭据存储目录发起任何 OpenFileById / GetFileAttributesExW / CreateFileW 请求，
//   从内核 I/O 层彻底避免触发安全软件「浏览器密码保护盾」告警。
//
// 版权声明: Copyright (c) 2026 Yy1 (yuan278501381). All rights reserved.
// ─────────────────────────────────────────────────────────────────────────────

#ifndef TOOLS3000_SERVICE_SENSITIVEPATHGUARD_H
#define TOOLS3000_SERVICE_SENSITIVEPATHGUARD_H

#include <array>
#include <string>
#include <string_view>

namespace tools3000::service {

class SensitivePathGuard {
public:
    static bool equalsAsciiIgnoreCase(std::wstring_view a, std::wstring_view b) noexcept {
        if (a.size() != b.size()) return false;
        for (size_t i = 0; i < a.size(); ++i) {
            wchar_t ca = a[i];
            wchar_t cb = b[i];
            if (ca >= L'A' && ca <= L'Z') ca = static_cast<wchar_t>(ca - L'A' + L'a');
            if (cb >= L'A' && cb <= L'Z') cb = static_cast<wchar_t>(cb - L'A' + L'a');
            if (ca != cb) return false;
        }
        return true;
    }

    static bool isSeparator(wchar_t ch) noexcept {
        return ch == L'\\' || ch == L'/';
    }

    /// 零分配快速判定文件名是否为浏览器密码/Cookie/密钥或系统机密数据库
    static bool isSensitiveCredentialFileName(std::wstring_view fileName) noexcept {
        if (fileName.empty() || fileName.size() > 64) {
            return false;
        }
        static constexpr std::array<std::wstring_view, 29> kSensitiveNames = {{
            // Chromium / Chrome / Edge / Brave / Vivaldi / Opera / Yandex
            L"login data",
            L"login data-journal",
            L"login data-wal",
            L"login data-shm",
            L"login data for account",
            L"login data for account-journal",
            L"login data for account-wal",
            L"login data for account-shm",
            L"web data",
            L"web data-journal",
            L"cookies",
            L"cookies-journal",
            L"local state",
            L"ya passman data",
            L"ya passman data-journal",
            // Firefox / Waterfox / LibreWolf / Zen / Thunderbird
            L"logins.json",
            L"logins-backup.json",
            L"logins-backup.json.tmp",
            L"key4.db",
            L"key3.db",
            L"cert9.db",
            L"cert8.db",
            L"cookies.sqlite",
            L"cookies.sqlite-wal",
            L"cookies.sqlite-shm",
            L"formhistory.sqlite",
            L"signons.sqlite",
            // Opera legacy & generic credential vaults
            L"wand.dat",
            L"vault.db"
        }};
        for (const auto candidate : kSensitiveNames) {
            if (equalsAsciiIgnoreCase(fileName, candidate)) {
                return true;
            }
        }
        return false;
    }

    /// 判定完整目录或文件路径是否位于主流浏览器用户配置数据或 Windows 系统凭据目录中（严格边界匹配）
    static bool isSensitiveCredentialDirectoryPath(std::wstring_view fullPath) noexcept {
        if (fullPath.size() < 12) {
            return false;
        }
        std::wstring lower;
        lower.reserve(fullPath.size());
        for (wchar_t ch : fullPath) {
            if (ch == L'/') {
                lower.push_back(L'\\');
            } else if (ch >= L'A' && ch <= L'Z') {
                lower.push_back(static_cast<wchar_t>(ch - L'A' + L'a'));
            } else {
                lower.push_back(ch);
            }
        }

        static constexpr std::array<std::wstring_view, 30> kSensitiveDirMarkers = {{
            // Chromium family
            L"\\google\\chrome\\user data",
            L"\\google\\chrome beta\\user data",
            L"\\google\\chrome dev\\user data",
            L"\\google\\chrome sxs\\user data",
            L"\\microsoft\\edge\\user data",
            L"\\microsoft\\edge beta\\user data",
            L"\\microsoft\\edge dev\\user data",
            L"\\microsoft\\edge sxs\\user data",
            L"\\bravesoftware\\brave-browser\\user data",
            L"\\vivaldi\\user data",
            L"\\chromium\\user data",
            L"\\opera software\\opera stable",
            L"\\opera software\\opera gx stable",
            L"\\opera software\\opera next",
            L"\\yandex\\yandexbrowser\\user data",
            L"\\360chrome\\chrome\\user data",
            L"\\360se6\\user data",
            L"\\centbrowser\\user data",
            L"\\arc\\user data",
            L"\\tor browser",
            // Gecko / Firefox family
            L"\\mozilla\\firefox\\profiles",
            L"\\waterfox\\profiles",
            L"\\librewolf\\profiles",
            L"\\zen\\profiles",
            L"\\pale moon\\profiles",
            L"\\thunderbird\\profiles",
            // Windows Credential Manager / DPAPI / Vault
            L"\\microsoft\\credentials",
            L"\\microsoft\\vault",
            L"\\microsoft\\protect",
            L"\\microsoft\\crypto"
        }};

        for (const auto marker : kSensitiveDirMarkers) {
            size_t pos = lower.find(marker);
            while (pos != std::wstring::npos) {
                const size_t after = pos + marker.size();
                if (after == lower.size() || lower[after] == L'\\') {
                    return true;
                }
                pos = lower.find(marker, pos + 1);
            }
        }
        return false;
    }

    /// 从完整路径提取末尾文件名并联合文件名与目录规则判断是否应跳过所有磁盘句柄触碰
    static bool isSensitiveCredentialPath(std::wstring_view fullPath) noexcept {
        if (fullPath.empty()) return false;
        size_t lastSep = fullPath.find_last_of(L"\\/");
        const std::wstring_view tail = (lastSep == std::wstring_view::npos)
            ? fullPath
            : fullPath.substr(lastSep + 1);
        return isSensitiveCredentialFileName(tail) || isSensitiveCredentialDirectoryPath(fullPath);
    }

    static bool shouldSkipMetadataHandleTouch(std::wstring_view fileName, std::wstring_view fullOrParentPath) noexcept {
        return isSensitiveCredentialFileName(fileName) || isSensitiveCredentialDirectoryPath(fullOrParentPath);
    }
};

} // namespace tools3000::service

#endif // TOOLS3000_SERVICE_SENSITIVEPATHGUARD_H
