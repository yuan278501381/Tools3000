#pragma once

// 每个 Windows 用户拥有独立、不可预测的搜索服务端点。SID 用于 DACL，随机
// token 用于防止其他本地用户仅靠猜到固定管道名就伪造服务或读取搜索结果。

#include <optional>
#include <string>
#include <string_view>

namespace tools3000::service::pipe_endpoint {

constexpr std::string_view TokenPrefix = "Tools3000SearchPipe-";
constexpr size_t TokenHexLength = 32;  // 128 bit

inline bool isValidToken(std::string_view token) noexcept {
    if (token.size() != TokenHexLength) return false;
    for (const char c : token) {
        if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) return false;
    }
    return true;
}

inline std::optional<std::string> makePipeName(std::string_view token) {
    if (!isValidToken(token)) return std::nullopt;
    return R"(\\.\pipe\)" + std::string(TokenPrefix) + std::string(token);
}

inline std::optional<std::wstring> makeSecurityDescriptor(std::wstring_view clientSid) {
    // SYSTEM is the server identity for SCM mode. Only the selected interactive
    // user receives read/write rights; Administrators deliberately are not a
    // normal data-plane principal (they can still take ownership by OS design).
    if (clientSid.empty()) return std::nullopt;
    return L"D:(A;;GA;;;SY)(A;;GRGW;;;" + std::wstring(clientSid) + L")";
}

}  // namespace tools3000::service::pipe_endpoint
