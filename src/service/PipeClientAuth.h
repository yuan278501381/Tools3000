#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <sddl.h>
#include <shlobj.h>
#include <cwctype>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace tools3000::service::pipe_auth {

inline wchar_t foldAsciiOrWideLower(wchar_t ch) noexcept {
    if (ch == L'/') return L'\\';
    if (ch >= L'A' && ch <= L'Z') return static_cast<wchar_t>(ch + (L'a' - L'A'));
    if (ch < 0x80) return ch;
    wchar_t folded = ch;
    CharLowerBuffW(&folded, 1);
    return folded;
}

inline std::wstring normalizeDirectoryPrefixLower(std::wstring_view rawPath) {
    if (rawPath.empty()) return {};
    std::wstring norm;
    norm.reserve(rawPath.size() + 1);
    for (const wchar_t ch : rawPath) {
        norm.push_back(foldAsciiOrWideLower(ch));
    }
    while (norm.size() > 3 && norm.back() == L'\\') {
        norm.pop_back();
    }
    if (!norm.empty() && norm.back() != L'\\') {
        norm.push_back(L'\\');
    }
    return norm;
}

inline bool equalsSidIgnoreCase(std::wstring_view lhs, std::wstring_view rhs) noexcept {
    if (lhs.size() != rhs.size()) return false;
    for (size_t i = 0; i < lhs.size(); ++i) {
        if (std::towupper(lhs[i]) != std::towupper(rhs[i])) {
            return false;
        }
    }
    return true;
}

inline bool equalsIgnoreCaseSlice(std::wstring_view lhs, std::wstring_view rhs) noexcept {
    if (lhs.size() != rhs.size()) return false;
    for (size_t i = 0; i < lhs.size(); ++i) {
        if (foldAsciiOrWideLower(lhs[i]) != foldAsciiOrWideLower(rhs[i])) {
            return false;
        }
    }
    return true;
}

inline std::wstring queryTokenUserSidString(HANDLE hToken) {
    if (!hToken || hToken == INVALID_HANDLE_VALUE) return {};
    DWORD needed = 0;
    GetTokenInformation(hToken, TokenUser, nullptr, 0, &needed);
    if (needed == 0) return {};

    std::vector<BYTE> buffer(needed);
    if (!GetTokenInformation(hToken, TokenUser, buffer.data(), needed, &needed)) {
        return {};
    }

    const auto* tokenUser = reinterpret_cast<const TOKEN_USER*>(buffer.data());
    if (!tokenUser->User.Sid) return {};

    LPWSTR rawSid = nullptr;
    if (!ConvertSidToStringSidW(tokenUser->User.Sid, &rawSid) || !rawSid) {
        return {};
    }
    std::wstring result(rawSid);
    LocalFree(rawSid);
    return result;
}

inline bool isTokenUserWellKnownSid(HANDLE hToken, WELL_KNOWN_SID_TYPE sidType) noexcept {
    if (!hToken || hToken == INVALID_HANDLE_VALUE) return false;
    DWORD needed = 0;
    GetTokenInformation(hToken, TokenUser, nullptr, 0, &needed);
    if (needed == 0) return false;

    std::vector<BYTE> buffer(needed);
    if (!GetTokenInformation(hToken, TokenUser, buffer.data(), needed, &needed)) {
        return false;
    }
    const auto* tokenUser = reinterpret_cast<const TOKEN_USER*>(buffer.data());
    return tokenUser->User.Sid && IsWellKnownSid(tokenUser->User.Sid, sidType) != FALSE;
}

inline bool isCurrentProcessLocalSystem() noexcept {
    HANDLE hProcessToken = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &hProcessToken)) {
        return false;
    }
    const bool isSys = isTokenUserWellKnownSid(hProcessToken, WinLocalSystemSid);
    CloseHandle(hProcessToken);
    return isSys;
}

inline std::wstring queryCurrentProcessUserSidString() {
    HANDLE hProcessToken = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &hProcessToken)) {
        return {};
    }
    std::wstring sid = queryTokenUserSidString(hProcessToken);
    CloseHandle(hProcessToken);
    return sid;
}

inline bool queryTokenImpersonationLevelAtLeastImpersonation(HANDLE hThreadToken) noexcept {
    SECURITY_IMPERSONATION_LEVEL level = SecurityAnonymous;
    DWORD returned = 0;
    if (!GetTokenInformation(hThreadToken, TokenImpersonationLevel, &level, sizeof(level), &returned)) {
        return false;
    }
    return level == SecurityImpersonation || level == SecurityDelegation;
}

inline bool queryTokenIntegrityAtLeastMedium(HANDLE hToken) noexcept {
    if (!hToken || hToken == INVALID_HANDLE_VALUE) return false;
    DWORD needed = 0;
    GetTokenInformation(hToken, TokenIntegrityLevel, nullptr, 0, &needed);
    if (needed == 0) return false;

    std::vector<BYTE> buffer(needed);
    if (!GetTokenInformation(hToken, TokenIntegrityLevel, buffer.data(), needed, &needed)) {
        return false;
    }
    const auto* til = reinterpret_cast<const TOKEN_MANDATORY_LABEL*>(buffer.data());
    if (!til || !til->Label.Sid) return false;
    const UCHAR subAuthCount = *GetSidSubAuthorityCount(til->Label.Sid);
    if (subAuthCount == 0) return false;
    const DWORD rid = *GetSidSubAuthority(til->Label.Sid, static_cast<DWORD>(subAuthCount - 1));
    return rid >= SECURITY_MANDATORY_MEDIUM_RID;
}

inline bool checkTokenIsAdministrator(HANDLE hImpersonationToken) noexcept {
    if (!hImpersonationToken || hImpersonationToken == INVALID_HANDLE_VALUE) return false;
    SID_IDENTIFIER_AUTHORITY ntAuthority = SECURITY_NT_AUTHORITY;
    PSID adminGroupSid = nullptr;
    if (!AllocateAndInitializeSid(
            &ntAuthority,
            2,
            SECURITY_BUILTIN_DOMAIN_RID,
            DOMAIN_ALIAS_RID_ADMINS,
            0, 0, 0, 0, 0, 0,
            &adminGroupSid)) {
        return false;
    }
    BOOL isMember = FALSE;
    const BOOL ok = CheckTokenMembership(hImpersonationToken, adminGroupSid, &isMember);
    FreeSid(adminGroupSid);
    return ok != FALSE && isMember != FALSE;
}

inline bool checkCurrentProcessIsAdministrator() noexcept {
    HANDLE hProcToken = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY | TOKEN_DUPLICATE, &hProcToken)) {
        return false;
    }
    HANDLE hImpToken = nullptr;
    const BOOL dupOk = DuplicateToken(hProcToken, SecurityIdentification, &hImpToken);
    CloseHandle(hProcToken);
    if (!dupOk || !hImpToken) {
        return false;
    }
    const bool isAdmin = checkTokenIsAdministrator(hImpToken);
    CloseHandle(hImpToken);
    return isAdmin;
}

inline bool checkTokenHasNetworkSid(HANDLE hImpersonationToken) noexcept {
    if (!hImpersonationToken || hImpersonationToken == INVALID_HANDLE_VALUE) return false;
    BYTE networkSidBuf[SECURITY_MAX_SID_SIZE]{};
    DWORD sidSize = sizeof(networkSidBuf);
    if (!CreateWellKnownSid(WinNetworkSid, nullptr, networkSidBuf, &sidSize)) {
        return false;
    }
    BOOL isMember = FALSE;
    if (!CheckTokenMembership(hImpersonationToken, networkSidBuf, &isMember)) {
        return false;
    }
    return isMember != FALSE;
}

inline std::wstring querySystemUsersRootPrefix() {
    static const std::wstring s_cachedUsersRoot = []() -> std::wstring {
        PWSTR rawProfilesDir = nullptr;
        if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_UserProfiles, KF_FLAG_DEFAULT, nullptr, &rawProfilesDir)) &&
            rawProfilesDir != nullptr) {
            std::wstring dir(rawProfilesDir);
            CoTaskMemFree(rawProfilesDir);
            if (!dir.empty()) {
                return normalizeDirectoryPrefixLower(dir);
            }
        }
        wchar_t sysDrive[64]{};
        const DWORD envLen = GetEnvironmentVariableW(L"SystemDrive", sysDrive, 60);
        if (envLen > 0 && envLen < 60) {
            return normalizeDirectoryPrefixLower(std::wstring(sysDrive, envLen) + L"\\Users");
        }
        return L"c:\\users\\";
    }();
    return s_cachedUsersRoot;
}

struct PipeClientAuthSession {
    HANDLE clientToken = nullptr;
    bool isSameUserProcessSid = false;
    bool authorized = false;
    bool isAdmin = false;
    DWORD lastError = ERROR_SUCCESS;
    std::wstring clientSid;
    std::wstring userProfilePrefix;
    std::wstring usersRootPrefix;

    PipeClientAuthSession() = default;
    ~PipeClientAuthSession() {
        reset();
    }

    PipeClientAuthSession(const PipeClientAuthSession&) = delete;
    PipeClientAuthSession& operator=(const PipeClientAuthSession&) = delete;

    PipeClientAuthSession(PipeClientAuthSession&& other) noexcept
        : clientToken(std::exchange(other.clientToken, nullptr))
        , isSameUserProcessSid(std::exchange(other.isSameUserProcessSid, false))
        , authorized(std::exchange(other.authorized, false))
        , isAdmin(std::exchange(other.isAdmin, false))
        , lastError(std::exchange(other.lastError, ERROR_SUCCESS))
        , clientSid(std::move(other.clientSid))
        , userProfilePrefix(std::move(other.userProfilePrefix))
        , usersRootPrefix(std::move(other.usersRootPrefix)) {}

    PipeClientAuthSession& operator=(PipeClientAuthSession&& other) noexcept {
        if (this != &other) {
            reset();
            clientToken = std::exchange(other.clientToken, nullptr);
            isSameUserProcessSid = std::exchange(other.isSameUserProcessSid, false);
            authorized = std::exchange(other.authorized, false);
            isAdmin = std::exchange(other.isAdmin, false);
            lastError = std::exchange(other.lastError, ERROR_SUCCESS);
            clientSid = std::move(other.clientSid);
            userProfilePrefix = std::move(other.userProfilePrefix);
            usersRootPrefix = std::move(other.usersRootPrefix);
        }
        return *this;
    }

    void reset() noexcept {
        if (clientToken != nullptr) {
            CloseHandle(clientToken);
            clientToken = nullptr;
        }
        isSameUserProcessSid = false;
        authorized = false;
        isAdmin = false;
        lastError = ERROR_SUCCESS;
        clientSid.clear();
        userProfilePrefix.clear();
        usersRootPrefix.clear();
    }
};

inline void populateClientIsolationMetadata(PipeClientAuthSession& session) {
    session.usersRootPrefix = querySystemUsersRootPrefix();
    if (session.clientToken != nullptr) {
        session.isAdmin = checkTokenIsAdministrator(session.clientToken);
        PWSTR rawProfile = nullptr;
        if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_Profile, KF_FLAG_DEFAULT, session.clientToken, &rawProfile)) &&
            rawProfile != nullptr) {
            session.userProfilePrefix = normalizeDirectoryPrefixLower(rawProfile);
            CoTaskMemFree(rawProfile);
        }
        if (session.userProfilePrefix.empty()) {
            wchar_t legacyBuf[MAX_PATH]{};
            if (SUCCEEDED(SHGetFolderPathW(nullptr, CSIDL_PROFILE, session.clientToken, 0, legacyBuf)) &&
                legacyBuf[0] != L'\0') {
                session.userProfilePrefix = normalizeDirectoryPrefixLower(legacyBuf);
            }
        }
    } else if (session.isSameUserProcessSid) {
        session.isAdmin = checkCurrentProcessIsAdministrator();
        PWSTR rawProfile = nullptr;
        if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_Profile, KF_FLAG_DEFAULT, nullptr, &rawProfile)) &&
            rawProfile != nullptr) {
            session.userProfilePrefix = normalizeDirectoryPrefixLower(rawProfile);
            CoTaskMemFree(rawProfile);
        }
    }
}

// Microsecond prefix filter enforcing cross-account private filename isolation (`HIGH-09`):
// Non-administrator standard users may search system-wide public paths and their own profile
// (`C:\Users\<Self>\`), plus shared profiles (`Public`, `Default`), while paths belonging to
// any other local user profile under `C:\Users\<Other>\` are strictly excluded.
inline bool isPathAllowedForClient(
    std::wstring_view fullPath,
    const PipeClientAuthSession& auth
) noexcept {
    if (!auth.authorized) {
        return false;
    }
    if (auth.isAdmin || auth.usersRootPrefix.empty()) {
        return true;
    }
    if (fullPath.size() < auth.usersRootPrefix.size()) {
        // Check exact `C:\Users` directory itself (without trailing slash)
        return true;
    }

    for (size_t i = 0; i < auth.usersRootPrefix.size(); ++i) {
        if (foldAsciiOrWideLower(fullPath[i]) != auth.usersRootPrefix[i]) {
            return true;  // Not inside `C:\Users\...`
        }
    }

    const std::wstring_view rest = fullPath.substr(auth.usersRootPrefix.size());
    if (rest.empty()) {
        return true;
    }

    size_t accountEnd = 0;
    while (accountEnd < rest.size() && rest[accountEnd] != L'\\' && rest[accountEnd] != L'/') {
        ++accountEnd;
    }
    const std::wstring_view accountSegment = rest.substr(0, accountEnd);
    if (accountSegment.empty()) {
        return true;
    }

    // Allow shared Windows profile directories
    if (equalsIgnoreCaseSlice(accountSegment, L"Public") ||
        equalsIgnoreCaseSlice(accountSegment, L"Default") ||
        equalsIgnoreCaseSlice(accountSegment, L"Default User") ||
        equalsIgnoreCaseSlice(accountSegment, L"All Users")) {
        return true;
    }

    // Allow the client's own user profile directory (exact directory or any descendant)
    if (!auth.userProfilePrefix.empty() && auth.userProfilePrefix.size() > auth.usersRootPrefix.size()) {
        std::wstring_view selfRel = std::wstring_view(auth.userProfilePrefix).substr(auth.usersRootPrefix.size());
        if (!selfRel.empty() && selfRel.back() == L'\\') {
            selfRel.remove_suffix(1);
        }
        if (!selfRel.empty() && equalsIgnoreCaseSlice(accountSegment, selfRel)) {
            return true;
        }
    }

    return false;
}

// Must ONLY be invoked AFTER the server has completed at least one ReadFile on hPipe.
// Calling ImpersonateNamedPipeClient before reading initial client data fails in the
// Windows kernel with ERROR_CANNOT_IMPERSONATE (1368).
inline PipeClientAuthSession authenticatePipeClientAfterFirstRead(
    HANDLE hPipe,
    std::wstring_view expectedClientSid = {}
) {
    PipeClientAuthSession session;
    const std::wstring processSid = queryCurrentProcessUserSidString();

    // 1. Portable / same-user fast path when expectedClientSid matches current non-SYSTEM process:
    if (!expectedClientSid.empty() && !processSid.empty() && !isCurrentProcessLocalSystem() &&
        equalsSidIgnoreCase(processSid, expectedClientSid)) {
        session.isSameUserProcessSid = true;
        session.authorized = true;
        session.clientSid = processSid;
        populateClientIsolationMetadata(session);
        return session;
    }

    // 2. Kernel named-pipe client impersonation channel:
    if (!ImpersonateNamedPipeClient(hPipe)) {
        session.lastError = GetLastError();
        session.authorized = false;
        return session;
    }

    struct RevertGuard {
        ~RevertGuard() {
            RevertToSelf();
        }
    } revertGuard;

    HANDLE hThreadToken = nullptr;
    if (!OpenThreadToken(
            GetCurrentThread(),
            TOKEN_DUPLICATE | TOKEN_QUERY | TOKEN_IMPERSONATE,
            TRUE,
            &hThreadToken)) {
        // Non-elevated portable/unit-test server without SeImpersonatePrivilege may hold an
        // identification-level thread token that supports TOKEN_QUERY | TOKEN_DUPLICATE.
        if (!OpenThreadToken(GetCurrentThread(), TOKEN_DUPLICATE | TOKEN_QUERY, TRUE, &hThreadToken)) {
            session.lastError = GetLastError();
            session.authorized = false;
            return session;
        }
    }

    struct HandleCloser {
        HANDLE h;
        ~HandleCloser() {
            if (h) CloseHandle(h);
        }
    } threadTokenCloser{hThreadToken};

    // Reject anonymous or low/untrusted integrity tokens unconditionally.
    if (isTokenUserWellKnownSid(hThreadToken, WinAnonymousSid) ||
        !queryTokenIntegrityAtLeastMedium(hThreadToken)) {
        session.lastError = ERROR_ACCESS_DENIED;
        session.authorized = false;
        return session;
    }

    const std::wstring clientSid = queryTokenUserSidString(hThreadToken);
    if (clientSid.empty()) {
        session.lastError = ERROR_ACCESS_DENIED;
        session.authorized = false;
        return session;
    }

    if (!expectedClientSid.empty() && !equalsSidIgnoreCase(clientSid, expectedClientSid)) {
        session.lastError = ERROR_ACCESS_DENIED;
        session.authorized = false;
        return session;
    }

    // Same-user non-SYSTEM server (e.g. portable mode or unprivileged unit test):
    // even if lack of SeImpersonatePrivilege capped the thread token at SecurityIdentification,
    // kernel-verified clientSid == processSid proves the caller is the exact same user.
    if (!processSid.empty() && !isCurrentProcessLocalSystem() && equalsSidIgnoreCase(clientSid, processSid)) {
        session.isSameUserProcessSid = true;
        session.authorized = true;
        session.clientSid = clientSid;
        populateClientIsolationMetadata(session);
        return session;
    }

    // Cross-account / LocalSystem SCM service MUST obtain full SecurityImpersonation+ token,
    // belong to a valid interactive user session (or Administrator), and not be a network logon.
    if (!queryTokenImpersonationLevelAtLeastImpersonation(hThreadToken)) {
        session.lastError = ERROR_BAD_IMPERSONATION_LEVEL;
        session.authorized = false;
        return session;
    }

    if (checkTokenHasNetworkSid(hThreadToken)) {
        session.lastError = ERROR_ACCESS_DENIED;
        session.authorized = false;
        return session;
    }

    DWORD tokenSessionId = 0;
    DWORD sessionRet = 0;
    const bool hasSessionId = GetTokenInformation(
        hThreadToken, TokenSessionId, &tokenSessionId, sizeof(tokenSessionId), &sessionRet) != FALSE;
    const bool isClientAdmin = checkTokenIsAdministrator(hThreadToken);
    if (!isClientAdmin && (!hasSessionId || tokenSessionId == 0)) {
        session.lastError = ERROR_ACCESS_DENIED;
        session.authorized = false;
        return session;
    }

    HANDLE hDupToken = nullptr;
    if (!DuplicateTokenEx(
            hThreadToken,
            TOKEN_QUERY | TOKEN_IMPERSONATE | TOKEN_DUPLICATE,
            nullptr,
            SecurityImpersonation,
            TokenImpersonation,
            &hDupToken)) {
        session.lastError = GetLastError();
        session.authorized = false;
        return session;
    }

    session.clientToken = hDupToken;
    session.clientSid = clientSid;
    session.authorized = true;
    populateClientIsolationMetadata(session);
    return session;
}

class ScopedContentReadImpersonation {
public:
    ScopedContentReadImpersonation(
        HANDLE hClientToken,
        bool isSameUserProcessSid,
        std::wstring_view expectedClientSid = {}
    ) noexcept {
        if (hClientToken != nullptr) {
            if (SetThreadToken(nullptr, hClientToken)) {
                m_impersonating = true;
                m_authorized = true;
            } else {
                m_lastError = GetLastError();
            }
            return;
        }

        if (isSameUserProcessSid) {
            m_authorized = true;
            return;
        }

        // Only allow ambient un-impersonated reads in non-SYSTEM unit-test harnesses where no
        // client SID was bound; LocalSystem service workers MUST always hold a client token.
        if (expectedClientSid.empty() && !isCurrentProcessLocalSystem()) {
            m_authorized = true;
            return;
        }

        m_lastError = ERROR_ACCESS_DENIED;
        m_authorized = false;
    }

    ~ScopedContentReadImpersonation() {
        if (m_impersonating) {
            RevertToSelf();
        }
    }

    ScopedContentReadImpersonation(const ScopedContentReadImpersonation&) = delete;
    ScopedContentReadImpersonation& operator=(const ScopedContentReadImpersonation&) = delete;

    bool isAuthorized() const noexcept { return m_authorized; }
    DWORD lastError() const noexcept { return m_lastError; }

private:
    bool m_impersonating = false;
    bool m_authorized = false;
    DWORD m_lastError = ERROR_SUCCESS;
};

} // namespace tools3000::service::pipe_auth
