#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <sddl.h>
#include <cwctype>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace tools3000::service::pipe_auth {

inline bool equalsSidIgnoreCase(std::wstring_view lhs, std::wstring_view rhs) noexcept {
    if (lhs.size() != rhs.size()) return false;
    for (size_t i = 0; i < lhs.size(); ++i) {
        if (std::towupper(lhs[i]) != std::towupper(rhs[i])) {
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

struct PipeClientAuthSession {
    HANDLE clientToken = nullptr;
    bool isSameUserProcessSid = false;
    bool authorized = false;
    DWORD lastError = ERROR_SUCCESS;

    PipeClientAuthSession() = default;
    ~PipeClientAuthSession() {
        reset();
    }

    PipeClientAuthSession(const PipeClientAuthSession&) = delete;
    PipeClientAuthSession& operator=(const PipeClientAuthSession&) = delete;

    PipeClientAuthSession(PipeClientAuthSession&& other) noexcept
        : clientToken(std::exchange(other.clientToken, nullptr)),
          isSameUserProcessSid(std::exchange(other.isSameUserProcessSid, false)),
          authorized(std::exchange(other.authorized, false)),
          lastError(std::exchange(other.lastError, ERROR_SUCCESS)) {}

    PipeClientAuthSession& operator=(PipeClientAuthSession&& other) noexcept {
        if (this != &other) {
            reset();
            clientToken = std::exchange(other.clientToken, nullptr);
            isSameUserProcessSid = std::exchange(other.isSameUserProcessSid, false);
            authorized = std::exchange(other.authorized, false);
            lastError = std::exchange(other.lastError, ERROR_SUCCESS);
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
        lastError = ERROR_SUCCESS;
    }
};

// Must ONLY be invoked AFTER the server has completed at least one ReadFile on hPipe.
// Calling ImpersonateNamedPipeClient before reading initial client data fails in the
// Windows kernel with ERROR_CANNOT_IMPERSONATE (1368).
inline PipeClientAuthSession authenticatePipeClientAfterFirstRead(
    HANDLE hPipe,
    std::wstring_view expectedClientSid
) {
    PipeClientAuthSession session;

    // 1. Portable / same-user fast path:
    // When Tools3000_Service runs under the exact same user SID as expectedClientSid
    // (and the pipe DACL already restricts access to SY + expectedClientSid), a non-elevated
    // user process may lack SeImpersonatePrivilege, causing kernel impersonation to cap at
    // SecurityIdentification. Reading disk directly as the current process is both safe and exact.
    if (!expectedClientSid.empty()) {
        const std::wstring processSid = queryCurrentProcessUserSidString();
        if (!processSid.empty() && equalsSidIgnoreCase(processSid, expectedClientSid)) {
            session.isSameUserProcessSid = true;
            session.authorized = true;
            return session;
        }
    }

    // 2. SCM LocalSystem (or cross-account) impersonation channel:
    if (!ImpersonateNamedPipeClient(hPipe)) {
        session.lastError = GetLastError();
        session.authorized = expectedClientSid.empty();
        return session;
    }

    struct RevertGuard {
        ~RevertGuard() {
            RevertToSelf();
        }
    } revertGuard;

    HANDLE hThreadToken = nullptr;
    if (!OpenThreadToken(GetCurrentThread(), TOKEN_DUPLICATE | TOKEN_QUERY | TOKEN_IMPERSONATE, TRUE, &hThreadToken)) {
        session.lastError = GetLastError();
        session.authorized = expectedClientSid.empty();
        return session;
    }

    struct HandleCloser {
        HANDLE h;
        ~HandleCloser() {
            if (h) CloseHandle(h);
        }
    } threadTokenCloser{hThreadToken};

    if (!queryTokenImpersonationLevelAtLeastImpersonation(hThreadToken)) {
        session.lastError = ERROR_BAD_IMPERSONATION_LEVEL;
        session.authorized = expectedClientSid.empty();
        return session;
    }

    if (!expectedClientSid.empty()) {
        const std::wstring clientSid = queryTokenUserSidString(hThreadToken);
        if (clientSid.empty() || !equalsSidIgnoreCase(clientSid, expectedClientSid)) {
            session.lastError = ERROR_ACCESS_DENIED;
            session.authorized = false;
            return session;
        }
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
        session.authorized = expectedClientSid.empty();
        return session;
    }

    session.clientToken = hDupToken;
    session.authorized = true;
    return session;
}

class ScopedContentReadImpersonation {
public:
    ScopedContentReadImpersonation(
        HANDLE hClientToken,
        bool isSameUserProcessSid,
        std::wstring_view expectedClientSid
    ) noexcept {
        if (hClientToken != nullptr) {
            if (SetThreadToken(nullptr, hClientToken)) {
                m_impersonated = true;
                m_authorized = true;
            } else {
                m_lastError = GetLastError();
                m_authorized = false;
            }
        } else if (expectedClientSid.empty() || isSameUserProcessSid) {
            m_authorized = true;
        } else {
            m_lastError = ERROR_ACCESS_DENIED;
            m_authorized = false;
        }
    }

    ~ScopedContentReadImpersonation() {
        if (m_impersonated) {
            SetThreadToken(nullptr, nullptr);
        }
    }

    ScopedContentReadImpersonation(const ScopedContentReadImpersonation&) = delete;
    ScopedContentReadImpersonation& operator=(const ScopedContentReadImpersonation&) = delete;

    bool isAuthorized() const noexcept { return m_authorized; }
    bool isImpersonatingThread() const noexcept { return m_impersonated; }
    DWORD lastError() const noexcept { return m_lastError; }

private:
    bool m_impersonated = false;
    bool m_authorized = false;
    DWORD m_lastError = ERROR_SUCCESS;
};

} // namespace tools3000::service::pipe_auth
