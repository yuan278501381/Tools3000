#pragma once
// ─────────────────────────────────────────────────────────────────────────────
// BackupCrypto.h — 原生 Windows CNG (BCrypt) 与 DPAPI 安全密码学原语
//
// 能力:
//   1. DPAPI 当前用户凭据加密 (CryptProtectData / CryptUnprotectData)；
//   2. SHA-256 / HMAC-SHA256 与工业级 AWS S3 Signature Version 4 规范签名；
//   3. AES-256-GCM + PBKDF2-HMAC-SHA256 客户端零知识端到端加密信封。
// ─────────────────────────────────────────────────────────────────────────────

#ifndef TOOLS3000_CORE_BACKUP_BACKUPCRYPTO_H
#define TOOLS3000_CORE_BACKUP_BACKUPCRYPTO_H

#include "core/utils/Export.h"

#include <cstdint>
#include <map>
#include <string>
#include <string_view>
#include <vector>

namespace tools3000::core::backup {

struct S3SigV4Params {
    std::string method;          // GET / PUT / DELETE / HEAD
    std::string host;            // host 或 host:port
    std::string canonicalUri;    // 以 '/' 开头，已按 RFC3986 逐段编码
    std::string canonicalQuery;  // 已按参数名升序排序并 RFC3986 编码
    std::string region;          // 如 "auto" / "us-east-1"
    std::string service = "s3";
    std::string accessKeyId;
    std::string secretAccessKey;
    std::string payloadSha256Hex;
    std::string amzDateUtc;      // YYYYMMDDTHHMMSSZ（为空时自动取当前 UTC）
    std::string contentType;     // 可选
};

struct S3SigV4Headers {
    std::string amzDate;
    std::string payloadSha256;
    std::string authorization;
    std::string signedHeaders;
};

class TOOLS3000CORE_API BackupCrypto final {
public:
    static std::string base64Encode(const uint8_t* data, size_t len);
    static bool base64Decode(std::string_view input, std::vector<uint8_t>& outBytes);

    static std::string sha256Hex(const uint8_t* data, size_t len);
    static std::string sha256Hex(std::string_view text);
    static std::vector<uint8_t> hmacSha256Raw(
        const uint8_t* key, size_t keyLen,
        const uint8_t* data, size_t dataLen);
    static std::string hmacSha256Hex(
        const uint8_t* key, size_t keyLen,
        const uint8_t* data, size_t dataLen);

    static bool generateRandomBytes(uint8_t* out, size_t len);

    /// 使用当前 Windows 登录用户 DPAPI 加密敏感凭据（返回 "dpapi:v1:<base64>"）
    static bool dpapiProtectUtf8(std::string_view plaintext, std::string& outProtected);
    static bool dpapiUnprotectUtf8(std::string_view protectedValue, std::string& outPlaintext);

    /// AES-256-GCM + PBKDF2-HMAC-SHA256 端到端加密信封（纯 JSON 文本）
    static bool encryptEnvelope(
        std::string_view plaintextUtf8,
        std::string_view passphraseUtf8,
        std::string& outEnvelopeJson,
        std::string& outError);

    /// 解密并认证端到端加密信封（任何 1 字节篡改或口令错误均严格拒绝）
    static bool decryptEnvelope(
        std::string_view envelopeJson,
        std::string_view passphraseUtf8,
        std::string& outPlaintextUtf8,
        std::string& outError);

    /// 按 RFC3986 编码 URI 路径段或查询参数（encodeSlash=false 保留 '/'）
    static std::string uriEncode(std::string_view raw, bool encodeSlash);

    /// 按 AWS Signature Version 4 规范生成完整 S3 请求签名头
    static S3SigV4Headers buildS3SigV4Headers(const S3SigV4Params& params);
};

}  // namespace tools3000::core::backup

#endif  // TOOLS3000_CORE_BACKUP_BACKUPCRYPTO_H
