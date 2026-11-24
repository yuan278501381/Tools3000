// ─────────────────────────────────────────────────────────────────────────────
// BackupCrypto.cpp — 原生 Windows CNG (BCrypt) 与 DPAPI 安全密码学实现
// ─────────────────────────────────────────────────────────────────────────────

#include "core/backup/BackupCrypto.h"
#include "core/backup/ConfigBackupTypes.h"

#include <windows.h>
#include <bcrypt.h>
#include <wincrypt.h>

#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <cstdio>
#include <cstring>
#include <ctime>

namespace tools3000::core::backup {

namespace {

constexpr const char* kHexLower = "0123456789abcdef";

std::string toHexLower(const uint8_t* data, size_t len) {
    std::string out;
    out.resize(len * 2u);
    for (size_t i = 0; i < len; ++i) {
        out[i * 2u] = kHexLower[(data[i] >> 4u) & 0x0Fu];
        out[i * 2u + 1u] = kHexLower[data[i] & 0x0Fu];
    }
    return out;
}

std::string currentUtcAmzDate() {
    SYSTEMTIME st{};
    GetSystemTime(&st);
    char buf[32]{};
    std::snprintf(buf, sizeof(buf), "%04u%02u%02uT%02u%02u%02uZ",
                  static_cast<unsigned>(st.wYear),
                  static_cast<unsigned>(st.wMonth),
                  static_cast<unsigned>(st.wDay),
                  static_cast<unsigned>(st.wHour),
                  static_cast<unsigned>(st.wMinute),
                  static_cast<unsigned>(st.wSecond));
    return std::string(buf);
}

bool deriveKeyPbkdf2Sha256(
    std::string_view passphrase,
    const uint8_t* salt, size_t saltLen,
    uint32_t iterations,
    uint8_t* outKey, size_t outKeyLen) {
    BCRYPT_ALG_HANDLE hAlg = nullptr;
    if (!BCRYPT_SUCCESS(BCryptOpenAlgorithmProvider(
            &hAlg, BCRYPT_SHA256_ALGORITHM, nullptr, BCRYPT_ALG_HANDLE_HMAC_FLAG))) {
        return false;
    }
    const NTSTATUS st = BCryptDeriveKeyPBKDF2(
        hAlg,
        reinterpret_cast<PUCHAR>(const_cast<char*>(passphrase.data())),
        static_cast<ULONG>(passphrase.size()),
        const_cast<PUCHAR>(salt),
        static_cast<ULONG>(saltLen),
        static_cast<ULONGLONG>(iterations),
        outKey,
        static_cast<ULONG>(outKeyLen),
        0);
    BCryptCloseAlgorithmProvider(hAlg, 0);
    return BCRYPT_SUCCESS(st);
}

}  // namespace

std::string BackupCrypto::base64Encode(const uint8_t* data, size_t len) {
    if (!data || len == 0) return {};
    static constexpr char kTable[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    out.reserve(((len + 2u) / 3u) * 4u);
    size_t i = 0;
    while (i + 2u < len) {
        const uint32_t v = (static_cast<uint32_t>(data[i]) << 16u) |
                           (static_cast<uint32_t>(data[i + 1u]) << 8u) |
                           static_cast<uint32_t>(data[i + 2u]);
        out.push_back(kTable[(v >> 18u) & 0x3Fu]);
        out.push_back(kTable[(v >> 12u) & 0x3Fu]);
        out.push_back(kTable[(v >> 6u) & 0x3Fu]);
        out.push_back(kTable[v & 0x3Fu]);
        i += 3u;
    }
    if (i < len) {
        const uint32_t b0 = data[i];
        const uint32_t b1 = (i + 1u < len) ? data[i + 1u] : 0u;
        out.push_back(kTable[(b0 >> 2u) & 0x3Fu]);
        out.push_back(kTable[((b0 & 0x03u) << 4u) | ((b1 >> 4u) & 0x0Fu)]);
        if (i + 1u < len) {
            out.push_back(kTable[(b1 & 0x0Fu) << 2u]);
        } else {
            out.push_back('=');
        }
        out.push_back('=');
    }
    return out;
}

bool BackupCrypto::base64Decode(std::string_view input, std::vector<uint8_t>& outBytes) {
    outBytes.clear();
    if (input.empty()) return true;
    static const auto kDecodeTable = []() {
        std::array<int8_t, 256> t{};
        t.fill(-1);
        for (int i = 'A'; i <= 'Z'; ++i) t[static_cast<size_t>(i)] = static_cast<int8_t>(i - 'A');
        for (int i = 'a'; i <= 'z'; ++i) t[static_cast<size_t>(i)] = static_cast<int8_t>(26 + i - 'a');
        for (int i = '0'; i <= '9'; ++i) t[static_cast<size_t>(i)] = static_cast<int8_t>(52 + i - '0');
        t[static_cast<size_t>('+')] = 62;
        t[static_cast<size_t>('/')] = 63;
        return t;
    }();

    uint32_t buf = 0;
    int bits = 0;
    outBytes.reserve((input.size() * 3u) / 4u);
    for (unsigned char ch : input) {
        if (ch == ' ' || ch == '\r' || ch == '\n' || ch == '\t') continue;
        if (ch == '=') break;
        const int8_t val = kDecodeTable[ch];
        if (val < 0) {
            outBytes.clear();
            return false;
        }
        buf = (buf << 6u) | static_cast<uint32_t>(val);
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            outBytes.push_back(static_cast<uint8_t>((buf >> static_cast<uint32_t>(bits)) & 0xFFu));
        }
    }
    return true;
}

std::string BackupCrypto::sha256Hex(const uint8_t* data, size_t len) {
    BCRYPT_ALG_HANDLE hAlg = nullptr;
    BCRYPT_HASH_HANDLE hHash = nullptr;
    std::array<uint8_t, 32> digest{};
    bool ok = false;
    if (BCRYPT_SUCCESS(BCryptOpenAlgorithmProvider(&hAlg, BCRYPT_SHA256_ALGORITHM, nullptr, 0))) {
        if (BCRYPT_SUCCESS(BCryptCreateHash(hAlg, &hHash, nullptr, 0, nullptr, 0, 0))) {
            if (len == 0 || BCRYPT_SUCCESS(BCryptHashData(
                    hHash, const_cast<PUCHAR>(data), static_cast<ULONG>(len), 0))) {
                ok = BCRYPT_SUCCESS(BCryptFinishHash(hHash, digest.data(), static_cast<ULONG>(digest.size()), 0));
            }
            BCryptDestroyHash(hHash);
        }
        BCryptCloseAlgorithmProvider(hAlg, 0);
    }
    return ok ? toHexLower(digest.data(), digest.size()) : std::string{};
}

std::string BackupCrypto::sha256Hex(std::string_view text) {
    return sha256Hex(reinterpret_cast<const uint8_t*>(text.data()), text.size());
}

std::vector<uint8_t> BackupCrypto::hmacSha256Raw(
    const uint8_t* key, size_t keyLen,
    const uint8_t* data, size_t dataLen) {
    BCRYPT_ALG_HANDLE hAlg = nullptr;
    BCRYPT_HASH_HANDLE hHash = nullptr;
    std::vector<uint8_t> digest(32, 0);
    bool ok = false;
    if (BCRYPT_SUCCESS(BCryptOpenAlgorithmProvider(
            &hAlg, BCRYPT_SHA256_ALGORITHM, nullptr, BCRYPT_ALG_HANDLE_HMAC_FLAG))) {
        if (BCRYPT_SUCCESS(BCryptCreateHash(
                hAlg, &hHash, nullptr, 0,
                const_cast<PUCHAR>(key), static_cast<ULONG>(keyLen), 0))) {
            if (dataLen == 0 || BCRYPT_SUCCESS(BCryptHashData(
                    hHash, const_cast<PUCHAR>(data), static_cast<ULONG>(dataLen), 0))) {
                ok = BCRYPT_SUCCESS(BCryptFinishHash(
                    hHash, digest.data(), static_cast<ULONG>(digest.size()), 0));
            }
            BCryptDestroyHash(hHash);
        }
        BCryptCloseAlgorithmProvider(hAlg, 0);
    }
    if (!ok) digest.clear();
    return digest;
}

std::string BackupCrypto::hmacSha256Hex(
    const uint8_t* key, size_t keyLen,
    const uint8_t* data, size_t dataLen) {
    const auto raw = hmacSha256Raw(key, keyLen, data, dataLen);
    return raw.empty() ? std::string{} : toHexLower(raw.data(), raw.size());
}

bool BackupCrypto::generateRandomBytes(uint8_t* out, size_t len) {
    if (!out || len == 0) return false;
    return BCRYPT_SUCCESS(BCryptGenRandom(
        nullptr, out, static_cast<ULONG>(len), BCRYPT_USE_SYSTEM_PREFERRED_RNG));
}

bool BackupCrypto::dpapiProtectUtf8(std::string_view plaintext, std::string& outProtected) {
    outProtected.clear();
    if (plaintext.empty()) {
        return true;
    }
    DATA_BLOB inBlob{};
    inBlob.pbData = reinterpret_cast<BYTE*>(const_cast<char*>(plaintext.data()));
    inBlob.cbData = static_cast<DWORD>(plaintext.size());
    DATA_BLOB outBlob{};
    if (!CryptProtectData(&inBlob, L"Tools3000BackupCredential", nullptr, nullptr, nullptr,
                          CRYPTPROTECT_UI_FORBIDDEN, &outBlob)) {
        return false;
    }
    outProtected = std::string(kDpapiPrefix) + base64Encode(outBlob.pbData, outBlob.cbData);
    SecureZeroMemory(outBlob.pbData, outBlob.cbData);
    LocalFree(outBlob.pbData);
    return true;
}

bool BackupCrypto::dpapiUnprotectUtf8(std::string_view protectedValue, std::string& outPlaintext) {
    outPlaintext.clear();
    if (protectedValue.empty()) return true;
    const std::string_view prefix(kDpapiPrefix);
    if (protectedValue.rfind(prefix, 0) != 0) {
        return false;
    }
    std::vector<uint8_t> raw;
    if (!base64Decode(protectedValue.substr(prefix.size()), raw) || raw.empty()) {
        return false;
    }
    DATA_BLOB inBlob{};
    inBlob.pbData = raw.data();
    inBlob.cbData = static_cast<DWORD>(raw.size());
    DATA_BLOB outBlob{};
    if (!CryptUnprotectData(&inBlob, nullptr, nullptr, nullptr, nullptr,
                            CRYPTPROTECT_UI_FORBIDDEN, &outBlob)) {
        return false;
    }
    outPlaintext.assign(reinterpret_cast<const char*>(outBlob.pbData), outBlob.cbData);
    SecureZeroMemory(outBlob.pbData, outBlob.cbData);
    LocalFree(outBlob.pbData);
    return true;
}

bool BackupCrypto::encryptEnvelope(
    std::string_view plaintextUtf8,
    std::string_view passphraseUtf8,
    std::string& outEnvelopeJson,
    std::string& outError) {
    outEnvelopeJson.clear();
    outError.clear();
    if (passphraseUtf8.empty()) {
        outError = "empty_passphrase";
        return false;
    }

    std::array<uint8_t, 16> salt{};
    std::array<uint8_t, 12> nonce{};
    std::array<uint8_t, 32> key{};
    std::array<uint8_t, 16> tag{};
    if (!generateRandomBytes(salt.data(), salt.size()) ||
        !generateRandomBytes(nonce.data(), nonce.size())) {
        outError = "rng_failed";
        return false;
    }
    if (!deriveKeyPbkdf2Sha256(passphraseUtf8, salt.data(), salt.size(),
                               kPbkdf2Iterations, key.data(), key.size())) {
        outError = "kdf_failed";
        return false;
    }

    BCRYPT_ALG_HANDLE hAlg = nullptr;
    BCRYPT_KEY_HANDLE hKey = nullptr;
    auto cleanup = [&]() {
        SecureZeroMemory(key.data(), key.size());
        if (hKey) BCryptDestroyKey(hKey);
        if (hAlg) BCryptCloseAlgorithmProvider(hAlg, 0);
    };

    if (!BCRYPT_SUCCESS(BCryptOpenAlgorithmProvider(&hAlg, BCRYPT_AES_ALGORITHM, nullptr, 0))) {
        cleanup();
        outError = "aes_open_failed";
        return false;
    }
    if (!BCRYPT_SUCCESS(BCryptSetProperty(
            hAlg, BCRYPT_CHAINING_MODE,
            reinterpret_cast<PUCHAR>(const_cast<wchar_t*>(BCRYPT_CHAIN_MODE_GCM)),
            static_cast<ULONG>((wcslen(BCRYPT_CHAIN_MODE_GCM) + 1u) * sizeof(wchar_t)), 0))) {
        cleanup();
        outError = "aes_gcm_mode_failed";
        return false;
    }
    if (!BCRYPT_SUCCESS(BCryptGenerateSymmetricKey(
            hAlg, &hKey, nullptr, 0, key.data(), static_cast<ULONG>(key.size()), 0))) {
        cleanup();
        outError = "aes_key_failed";
        return false;
    }

    BCRYPT_AUTHENTICATED_CIPHER_MODE_INFO authInfo{};
    BCRYPT_INIT_AUTH_MODE_INFO(authInfo);
    authInfo.pbNonce = nonce.data();
    authInfo.cbNonce = static_cast<ULONG>(nonce.size());
    authInfo.pbTag = tag.data();
    authInfo.cbTag = static_cast<ULONG>(tag.size());

    std::vector<uint8_t> ciphertext(plaintextUtf8.size());
    ULONG outLen = 0;
    const NTSTATUS st = BCryptEncrypt(
        hKey,
        reinterpret_cast<PUCHAR>(const_cast<char*>(plaintextUtf8.data())),
        static_cast<ULONG>(plaintextUtf8.size()),
        &authInfo,
        nullptr, 0,
        ciphertext.empty() ? nullptr : ciphertext.data(),
        static_cast<ULONG>(ciphertext.size()),
        &outLen, 0);
    cleanup();

    if (!BCRYPT_SUCCESS(st)) {
        outError = "aes_encrypt_failed";
        return false;
    }
    ciphertext.resize(outLen);

    const nlohmann::json env = {
        {"format", "tools3000-backup-envelope"},
        {"schemaVersion", kBackupSchemaVersion},
        {"encrypted", true},
        {"cipher", "aes-256-gcm"},
        {"kdf", "pbkdf2-hmac-sha256"},
        {"iterations", kPbkdf2Iterations},
        {"salt", base64Encode(salt.data(), salt.size())},
        {"nonce", base64Encode(nonce.data(), nonce.size())},
        {"tag", base64Encode(tag.data(), tag.size())},
        {"ciphertext", base64Encode(ciphertext.data(), ciphertext.size())},
    };
    outEnvelopeJson = env.dump();
    return true;
}

bool BackupCrypto::decryptEnvelope(
    std::string_view envelopeJson,
    std::string_view passphraseUtf8,
    std::string& outPlaintextUtf8,
    std::string& outError) {
    outPlaintextUtf8.clear();
    outError.clear();
    if (passphraseUtf8.empty()) {
        outError = "passphrase_required";
        return false;
    }

    nlohmann::json env;
    try {
        env = nlohmann::json::parse(envelopeJson);
    } catch (...) {
        outError = "invalid_envelope_json";
        return false;
    }
    if (!env.is_object() || !env.value("encrypted", false)) {
        outError = "not_encrypted_envelope";
        return false;
    }

    const uint32_t iterations = env.value("iterations", kPbkdf2Iterations);
    if (iterations < 10000u || iterations > 2000000u) {
        outError = "invalid_kdf_iterations";
        return false;
    }

    std::vector<uint8_t> salt, nonce, tag, ciphertext;
    if (!base64Decode(env.value("salt", ""), salt) || salt.size() < 8u ||
        !base64Decode(env.value("nonce", ""), nonce) || nonce.size() != 12u ||
        !base64Decode(env.value("tag", ""), tag) || tag.size() != 16u ||
        !base64Decode(env.value("ciphertext", ""), ciphertext)) {
        outError = "corrupted_envelope_fields";
        return false;
    }

    std::array<uint8_t, 32> key{};
    if (!deriveKeyPbkdf2Sha256(passphraseUtf8, salt.data(), salt.size(),
                               iterations, key.data(), key.size())) {
        outError = "kdf_failed";
        return false;
    }

    BCRYPT_ALG_HANDLE hAlg = nullptr;
    BCRYPT_KEY_HANDLE hKey = nullptr;
    auto cleanup = [&]() {
        SecureZeroMemory(key.data(), key.size());
        if (hKey) BCryptDestroyKey(hKey);
        if (hAlg) BCryptCloseAlgorithmProvider(hAlg, 0);
    };

    if (!BCRYPT_SUCCESS(BCryptOpenAlgorithmProvider(&hAlg, BCRYPT_AES_ALGORITHM, nullptr, 0)) ||
        !BCRYPT_SUCCESS(BCryptSetProperty(
            hAlg, BCRYPT_CHAINING_MODE,
            reinterpret_cast<PUCHAR>(const_cast<wchar_t*>(BCRYPT_CHAIN_MODE_GCM)),
            static_cast<ULONG>((wcslen(BCRYPT_CHAIN_MODE_GCM) + 1u) * sizeof(wchar_t)), 0)) ||
        !BCRYPT_SUCCESS(BCryptGenerateSymmetricKey(
            hAlg, &hKey, nullptr, 0, key.data(), static_cast<ULONG>(key.size()), 0))) {
        cleanup();
        outError = "aes_init_failed";
        return false;
    }

    BCRYPT_AUTHENTICATED_CIPHER_MODE_INFO authInfo{};
    BCRYPT_INIT_AUTH_MODE_INFO(authInfo);
    authInfo.pbNonce = nonce.data();
    authInfo.cbNonce = static_cast<ULONG>(nonce.size());
    authInfo.pbTag = tag.data();
    authInfo.cbTag = static_cast<ULONG>(tag.size());

    std::vector<uint8_t> plain(ciphertext.size());
    ULONG outLen = 0;
    const NTSTATUS st = BCryptDecrypt(
        hKey,
        ciphertext.empty() ? nullptr : ciphertext.data(),
        static_cast<ULONG>(ciphertext.size()),
        &authInfo,
        nullptr, 0,
        plain.empty() ? nullptr : plain.data(),
        static_cast<ULONG>(plain.size()),
        &outLen, 0);
    cleanup();

    if (!BCRYPT_SUCCESS(st)) {
        outError = "decrypt_auth_failed";
        return false;
    }
    outPlaintextUtf8.assign(reinterpret_cast<const char*>(plain.data()), outLen);
    return true;
}

std::string BackupCrypto::uriEncode(std::string_view raw, bool encodeSlash) {
    static constexpr char kHexUpper[] = "0123456789ABCDEF";
    std::string out;
    out.reserve(raw.size() + 16u);
    for (unsigned char ch : raw) {
        const bool unreserved =
            (ch >= 'A' && ch <= 'Z') ||
            (ch >= 'a' && ch <= 'z') ||
            (ch >= '0' && ch <= '9') ||
            ch == '_' || ch == '-' || ch == '~' || ch == '.';
        if (unreserved || (!encodeSlash && ch == '/')) {
            out.push_back(static_cast<char>(ch));
        } else {
            out.push_back('%');
            out.push_back(kHexUpper[(ch >> 4u) & 0x0Fu]);
            out.push_back(kHexUpper[ch & 0x0Fu]);
        }
    }
    return out;
}

S3SigV4Headers BackupCrypto::buildS3SigV4Headers(const S3SigV4Params& params) {
    S3SigV4Headers res{};
    res.amzDate = params.amzDateUtc.empty() ? currentUtcAmzDate() : params.amzDateUtc;
    res.payloadSha256 = params.payloadSha256Hex.empty()
        ? sha256Hex("")
        : params.payloadSha256Hex;

    const std::string dateStamp = res.amzDate.size() >= 8u ? res.amzDate.substr(0, 8u) : "19700101";
    const std::string region = params.region.empty() ? "auto" : params.region;
    const std::string service = params.service.empty() ? "s3" : params.service;

    std::map<std::string, std::string> canonicalMap;
    canonicalMap["host"] = params.host;
    canonicalMap["x-amz-content-sha256"] = res.payloadSha256;
    canonicalMap["x-amz-date"] = res.amzDate;
    if (!params.contentType.empty()) {
        canonicalMap["content-type"] = params.contentType;
    }

    std::string canonicalHeaders;
    std::string signedHeaders;
    for (const auto& [k, v] : canonicalMap) {
        canonicalHeaders += k + ":" + v + "\n";
        if (!signedHeaders.empty()) signedHeaders.push_back(';');
        signedHeaders += k;
    }

    const std::string canonicalUri = params.canonicalUri.empty() ? "/" : params.canonicalUri;
    const std::string canonicalRequest =
        params.method + "\n" +
        canonicalUri + "\n" +
        params.canonicalQuery + "\n" +
        canonicalHeaders + "\n" +
        signedHeaders + "\n" +
        res.payloadSha256;

    const std::string credentialScope = dateStamp + "/" + region + "/" + service + "/aws4_request";
    const std::string stringToSign =
        "AWS4-HMAC-SHA256\n" +
        res.amzDate + "\n" +
        credentialScope + "\n" +
        sha256Hex(canonicalRequest);

    const std::string kSecret = "AWS4" + params.secretAccessKey;
    const auto kDate = hmacSha256Raw(
        reinterpret_cast<const uint8_t*>(kSecret.data()), kSecret.size(),
        reinterpret_cast<const uint8_t*>(dateStamp.data()), dateStamp.size());
    const auto kRegion = hmacSha256Raw(
        kDate.data(), kDate.size(),
        reinterpret_cast<const uint8_t*>(region.data()), region.size());
    const auto kService = hmacSha256Raw(
        kRegion.data(), kRegion.size(),
        reinterpret_cast<const uint8_t*>(service.data()), service.size());
    const auto kSigning = hmacSha256Raw(
        kService.data(), kService.size(),
        reinterpret_cast<const uint8_t*>("aws4_request"), 12u);
    const std::string signature = hmacSha256Hex(
        kSigning.data(), kSigning.size(),
        reinterpret_cast<const uint8_t*>(stringToSign.data()), stringToSign.size());

    res.signedHeaders = signedHeaders;
    res.authorization =
        "AWS4-HMAC-SHA256 Credential=" + params.accessKeyId + "/" + credentialScope +
        ", SignedHeaders=" + signedHeaders +
        ", Signature=" + signature;
    return res;
}

}  // namespace tools3000::core::backup
