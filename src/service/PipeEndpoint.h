#pragma once

// 搜索服务命名管道端点与内核级安全描述符契约：
// 1. SCM LocalSystem 服务固定发布内核身份鉴权端点 `KernelAuthPipeName`，通过
//    Windows 内核不可伪造的命名管道客户端模拟令牌 (`ImpersonateNamedPipeClient`)
//    识别真实登录会话与用户 SID，彻底解耦易被重置/导入覆盖的配置文件随机字符串；
// 2. 便携模式按当前用户不可变 SID 派生独立管道名与专属 DACL；
// 3. 全量管道 SDDL 强制附加 `S:(ML;;NW;;;ME)` 中等完整性标签，在内核层直接阻断
//    Low Integrity / AppContainer 沙箱进程连接。

#include <optional>
#include <string>
#include <string_view>

namespace tools3000::service::pipe_endpoint {

inline constexpr std::string_view KernelAuthPipeName = R"(\\.\pipe\Tools3000SearchPipe_KernelAuth)";
inline constexpr std::string_view UserSidPipePrefix = "Tools3000SearchPipe_Sid_";
constexpr std::string_view TokenPrefix = "Tools3000SearchPipe-";
constexpr size_t TokenHexLength = 32;  // 128 bit

inline bool isValidToken(std::string_view token) noexcept {
    if (token.size() != TokenHexLength) return false;
    for (const char c : token) {
        if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) return false;
    }
    return true;
}

inline bool isSafeSidCharacters(std::wstring_view sid) noexcept {
    if (sid.size() < 4 || sid.size() > 128) return false;
    for (const wchar_t ch : sid) {
        const bool ok = (ch >= L'0' && ch <= L'9') ||
                        (ch >= L'A' && ch <= L'Z') ||
                        (ch >= L'a' && ch <= L'z') ||
                        ch == L'-';
        if (!ok) return false;
    }
    return true;
}

inline std::optional<std::string> makePipeName(std::string_view token) {
    if (!isValidToken(token)) return std::nullopt;
    return R"(\\.\pipe\)" + std::string(TokenPrefix) + std::string(token);
}

inline std::optional<std::string> makeUserSidPipeName(std::wstring_view clientSid) {
    if (!isSafeSidCharacters(clientSid)) return std::nullopt;
    std::string narrowSid;
    narrowSid.reserve(clientSid.size());
    for (const wchar_t ch : clientSid) {
        const char c = static_cast<char>((ch >= L'a' && ch <= L'z') ? (ch - (L'a' - L'A')) : ch);
        narrowSid.push_back(c);
    }
    return R"(\\.\pipe\)" + std::string(UserSidPipePrefix) + narrowSid;
}

inline std::optional<std::wstring> makeSecurityDescriptor(std::wstring_view clientSid) {
    // SYSTEM is the server identity for SCM mode. Only the selected interactive
    // user receives read/write rights; S:(ML;;NW;;;ME) blocks Low/AppContainer callers.
    if (clientSid.empty()) return std::nullopt;
    return L"D:(A;;GA;;;SY)(A;;GRGW;;;" + std::wstring(clientSid) + L")S:(ML;;NW;;;ME)";
}

inline std::wstring makeKernelServiceSecurityDescriptor() {
    // LocalSystem + Builtin Administrators full control; Interactive Users read/write;
    // Mandatory Integrity Label Medium+ (blocks Low/Untrusted/AppContainer & Network logon).
    return L"D:(A;;GA;;;SY)(A;;GA;;;BA)(A;;GRGW;;;IU)S:(ML;;NW;;;ME)";
}

}  // namespace tools3000::service::pipe_endpoint
