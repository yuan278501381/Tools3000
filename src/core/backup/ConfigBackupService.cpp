// ─────────────────────────────────────────────────────────────────────────────
// ConfigBackupService.cpp — 配置云备份领域服务实现
// ─────────────────────────────────────────────────────────────────────────────

#include "core/backup/ConfigBackupService.h"
#include "core/backup/BackupCrypto.h"
#include "core/backup/BackupTransport.h"
#include "core/config/ConfigManager.h"
#include "core/logger/Logger.h"
#include "core/utils/WinUtils.h"
#include "Tools3000Version.h"

#include <windows.h>

#include <algorithm>
#include <array>
#include <cstdio>
#include <fstream>
#include <unordered_set>

namespace tools3000::core::backup {

namespace {

constexpr const char* kCfgEnabled = "/backup/enabled";
constexpr const char* kCfgProvider = "/backup/provider";
constexpr const char* kCfgAutoOnExit = "/backup/autoBackupOnExit";
constexpr const char* kCfgKeepCount = "/backup/keepCount";
constexpr const char* kCfgEncryption = "/backup/encryptionEnabled";
constexpr const char* kCfgFolderPath = "/backup/folderPath";
constexpr const char* kCfgWebdavUrl = "/backup/webdavUrl";
constexpr const char* kCfgWebdavUser = "/backup/webdavUsername";
constexpr const char* kCfgWebdavPassEnc = "/backup/webdavPasswordEnc";
constexpr const char* kCfgS3Endpoint = "/backup/s3Endpoint";
constexpr const char* kCfgS3Region = "/backup/s3Region";
constexpr const char* kCfgS3Bucket = "/backup/s3Bucket";
constexpr const char* kCfgS3Prefix = "/backup/s3Prefix";
constexpr const char* kCfgS3AccessKey = "/backup/s3AccessKeyId";
constexpr const char* kCfgS3SecretEnc = "/backup/s3SecretKeyEnc";
constexpr const char* kCfgS3ForcePath = "/backup/s3ForcePathStyle";
constexpr const char* kCfgPassphraseEnc = "/backup/encryptionPassphraseEnc";
constexpr const char* kCfgLastAt = "/backup/lastBackupAtIso";
constexpr const char* kCfgLastStatus = "/backup/lastBackupStatus";
constexpr const char* kCfgLastFile = "/backup/lastBackupFile";

std::string currentUtcIsoString() {
    SYSTEMTIME st{};
    GetSystemTime(&st);
    char buf[32]{};
    std::snprintf(buf, sizeof(buf), "%04u-%02u-%02uT%02u:%02u:%02uZ",
                  static_cast<unsigned>(st.wYear),
                  static_cast<unsigned>(st.wMonth),
                  static_cast<unsigned>(st.wDay),
                  static_cast<unsigned>(st.wHour),
                  static_cast<unsigned>(st.wMinute),
                  static_cast<unsigned>(st.wSecond));
    return std::string(buf);
}

std::string currentLocalStampCompact() {
    SYSTEMTIME st{};
    GetLocalTime(&st);
    char buf[32]{};
    std::snprintf(buf, sizeof(buf), "%04u%02u%02u-%02u%02u%02u",
                  static_cast<unsigned>(st.wYear),
                  static_cast<unsigned>(st.wMonth),
                  static_cast<unsigned>(st.wDay),
                  static_cast<unsigned>(st.wHour),
                  static_cast<unsigned>(st.wMinute),
                  static_cast<unsigned>(st.wSecond));
    return std::string(buf);
}

std::string getLocalMachineNameUtf8() {
    wchar_t buf[MAX_COMPUTERNAME_LENGTH + 1]{};
    DWORD len = static_cast<DWORD>(std::size(buf));
    if (GetComputerNameW(buf, &len) && len > 0) {
        return WinUtils::wstringToUtf8(std::wstring(buf, len));
    }
    return "WindowsPC";
}

std::wstring getEnvVarW(const wchar_t* name) {
    wchar_t buf[MAX_PATH * 2]{};
    const DWORD len = GetEnvironmentVariableW(name, buf, static_cast<DWORD>(std::size(buf)));
    if (len == 0 || len >= std::size(buf)) return {};
    return std::wstring(buf, len);
}

std::string decryptStoredSecret(const std::string& stored) {
    if (stored.empty()) return {};
    std::string plain;
    if (BackupCrypto::dpapiUnprotectUtf8(stored, plain)) {
        return plain;
    }
    return {};
}

bool storeEncryptedSecret(const char* key, const std::string& plaintext) {
    auto& cfg = ConfigManager::instance();
    if (plaintext.empty()) {
        cfg.set(key, std::string{});
        return true;
    }
    std::string enc;
    if (!BackupCrypto::dpapiProtectUtf8(plaintext, enc)) {
        return false;
    }
    cfg.set(key, enc);
    return true;
}

bool atomicWriteUtf8File(const std::filesystem::path& target, const std::string& content) {
    std::error_code ec;
    if (target.has_parent_path()) {
        std::filesystem::create_directories(target.parent_path(), ec);
    }
    const std::filesystem::path tmp = target.wstring() + L".tmp";
    {
        std::ofstream ofs(tmp, std::ios::binary | std::ios::trunc);
        if (!ofs) return false;
        ofs.write(content.data(), static_cast<std::streamsize>(content.size()));
        if (!ofs.good()) return false;
    }
    std::filesystem::rename(tmp, target, ec);
    if (ec) {
        std::filesystem::remove(target, ec);
        ec.clear();
        std::filesystem::rename(tmp, target, ec);
        if (ec) {
            std::filesystem::remove(tmp, ec);
            return false;
        }
    }
    return true;
}

}  // namespace

ConfigBackupService& ConfigBackupService::instance() {
    static ConfigBackupService svc;
    return svc;
}

std::string ConfigBackupService::providerToString(BackupProviderType provider) {
    switch (provider) {
        case BackupProviderType::Folder: return "folder";
        case BackupProviderType::WebDav: return "webdav";
        case BackupProviderType::S3:     return "s3";
        case BackupProviderType::None:
        default:                         return "none";
    }
}

BackupProviderType ConfigBackupService::providerFromString(std::string_view value) {
    if (value == "folder") return BackupProviderType::Folder;
    if (value == "webdav") return BackupProviderType::WebDav;
    if (value == "s3") return BackupProviderType::S3;
    return BackupProviderType::None;
}

CloudBackupSettings ConfigBackupService::getSettings() const {
    auto& cfg = ConfigManager::instance();
    CloudBackupSettings s{};
    s.enabled = cfg.get<bool>(kCfgEnabled, false);
    s.provider = providerFromString(cfg.get<std::string>(kCfgProvider, "folder"));
    if (s.provider == BackupProviderType::None) s.provider = BackupProviderType::Folder;
    s.autoBackupOnExit = cfg.get<bool>(kCfgAutoOnExit, false);
    s.keepCount = std::clamp(cfg.get<int>(kCfgKeepCount, kDefaultKeepCount), kMinKeepCount, kMaxKeepCount);
    s.encryptionEnabled = cfg.get<bool>(kCfgEncryption, false);

    s.folderPath = cfg.get<std::string>(kCfgFolderPath, "");
    s.webdavUrl = cfg.get<std::string>(kCfgWebdavUrl, "");
    s.webdavUsername = cfg.get<std::string>(kCfgWebdavUser, "");
    s.hasWebdavPassword = !cfg.get<std::string>(kCfgWebdavPassEnc, "").empty();

    s.s3Endpoint = cfg.get<std::string>(kCfgS3Endpoint, "");
    s.s3Region = cfg.get<std::string>(kCfgS3Region, "auto");
    s.s3Bucket = cfg.get<std::string>(kCfgS3Bucket, "");
    s.s3Prefix = cfg.get<std::string>(kCfgS3Prefix, "tools3000-backups/");
    s.s3AccessKeyId = cfg.get<std::string>(kCfgS3AccessKey, "");
    s.s3ForcePathStyle = cfg.get<bool>(kCfgS3ForcePath, true);
    s.hasS3SecretKey = !cfg.get<std::string>(kCfgS3SecretEnc, "").empty();

    s.hasEncryptionPassphrase = !cfg.get<std::string>(kCfgPassphraseEnc, "").empty();

    s.lastBackupAtIso = cfg.get<std::string>(kCfgLastAt, "");
    s.lastBackupStatus = cfg.get<std::string>(kCfgLastStatus, "");
    s.lastBackupFile = cfg.get<std::string>(kCfgLastFile, "");
    return s;
}

bool ConfigBackupService::saveSettings(
    const CloudBackupSettings& next,
    const CloudBackupSecretUpdates& secrets) {
    std::lock_guard<std::mutex> lock(m_opMutex);
    auto& cfg = ConfigManager::instance();
    cfg.set(kCfgEnabled, next.enabled);
    cfg.set(kCfgProvider, providerToString(next.provider));
    cfg.set(kCfgAutoOnExit, next.autoBackupOnExit);
    cfg.set(kCfgKeepCount, std::clamp(next.keepCount, kMinKeepCount, kMaxKeepCount));
    cfg.set(kCfgEncryption, next.encryptionEnabled);

    cfg.set(kCfgFolderPath, next.folderPath);
    cfg.set(kCfgWebdavUrl, next.webdavUrl);
    cfg.set(kCfgWebdavUser, next.webdavUsername);
    if (secrets.updateWebdavPassword && !storeEncryptedSecret(kCfgWebdavPassEnc, secrets.webdavPassword)) {
        return false;
    }

    cfg.set(kCfgS3Endpoint, next.s3Endpoint);
    cfg.set(kCfgS3Region, next.s3Region.empty() ? std::string("auto") : next.s3Region);
    cfg.set(kCfgS3Bucket, next.s3Bucket);
    cfg.set(kCfgS3Prefix, next.s3Prefix);
    cfg.set(kCfgS3AccessKey, next.s3AccessKeyId);
    cfg.set(kCfgS3ForcePath, next.s3ForcePathStyle);
    if (secrets.updateS3SecretKey && !storeEncryptedSecret(kCfgS3SecretEnc, secrets.s3SecretKey)) {
        return false;
    }
    if (secrets.updateEncryptionPassphrase &&
        !storeEncryptedSecret(kCfgPassphraseEnc, secrets.encryptionPassphrase)) {
        return false;
    }
    return cfg.flush();
}

ResolvedBackupConfig ConfigBackupService::resolveRuntimeConfig(const nlohmann::json* draftOverride) const {
    auto& cfg = ConfigManager::instance();
    ResolvedBackupConfig out{};
    out.settings = getSettings();
    out.webdavPassword = decryptStoredSecret(cfg.get<std::string>(kCfgWebdavPassEnc, ""));
    out.s3SecretKey = decryptStoredSecret(cfg.get<std::string>(kCfgS3SecretEnc, ""));
    out.encryptionPassphrase = decryptStoredSecret(cfg.get<std::string>(kCfgPassphraseEnc, ""));

    if (draftOverride && draftOverride->is_object()) {
        const auto& d = *draftOverride;
        if (d.contains("provider") && d["provider"].is_string()) {
            out.settings.provider = providerFromString(d["provider"].get<std::string>());
        }
        if (d.contains("keepCount") && d["keepCount"].is_number_integer()) {
            out.settings.keepCount = std::clamp(d["keepCount"].get<int>(), kMinKeepCount, kMaxKeepCount);
        }
        if (d.contains("encryptionEnabled") && d["encryptionEnabled"].is_boolean()) {
            out.settings.encryptionEnabled = d["encryptionEnabled"].get<bool>();
        }
        if (d.contains("folderPath") && d["folderPath"].is_string()) {
            out.settings.folderPath = d["folderPath"].get<std::string>();
        }
        if (d.contains("webdavUrl") && d["webdavUrl"].is_string()) {
            out.settings.webdavUrl = d["webdavUrl"].get<std::string>();
        }
        if (d.contains("webdavUsername") && d["webdavUsername"].is_string()) {
            out.settings.webdavUsername = d["webdavUsername"].get<std::string>();
        }
        if (d.contains("webdavPassword") && d["webdavPassword"].is_string() && !d["webdavPassword"].get<std::string>().empty()) {
            out.webdavPassword = d["webdavPassword"].get<std::string>();
        }
        if (d.contains("s3Endpoint") && d["s3Endpoint"].is_string()) {
            out.settings.s3Endpoint = d["s3Endpoint"].get<std::string>();
        }
        if (d.contains("s3Region") && d["s3Region"].is_string()) {
            out.settings.s3Region = d["s3Region"].get<std::string>();
        }
        if (d.contains("s3Bucket") && d["s3Bucket"].is_string()) {
            out.settings.s3Bucket = d["s3Bucket"].get<std::string>();
        }
        if (d.contains("s3Prefix") && d["s3Prefix"].is_string()) {
            out.settings.s3Prefix = d["s3Prefix"].get<std::string>();
        }
        if (d.contains("s3AccessKeyId") && d["s3AccessKeyId"].is_string()) {
            out.settings.s3AccessKeyId = d["s3AccessKeyId"].get<std::string>();
        }
        if (d.contains("s3SecretKey") && d["s3SecretKey"].is_string() && !d["s3SecretKey"].get<std::string>().empty()) {
            out.s3SecretKey = d["s3SecretKey"].get<std::string>();
        }
        if (d.contains("s3ForcePathStyle") && d["s3ForcePathStyle"].is_boolean()) {
            out.settings.s3ForcePathStyle = d["s3ForcePathStyle"].get<bool>();
        }
        if (d.contains("encryptionPassphrase") && d["encryptionPassphrase"].is_string() &&
            !d["encryptionPassphrase"].get<std::string>().empty()) {
            out.encryptionPassphrase = d["encryptionPassphrase"].get<std::string>();
        }
    }
    return out;
}

std::vector<DetectedSyncFolder> ConfigBackupService::detectLocalSyncFolders() {
    const std::wstring userProfile = getEnvVarW(L"USERPROFILE");
    const std::wstring localAppData = getEnvVarW(L"LOCALAPPDATA");
    const std::wstring oneDriveEnv = getEnvVarW(L"OneDrive");
    const std::wstring oneDriveConsumer = getEnvVarW(L"OneDriveConsumer");
    const std::wstring oneDriveCommercial = getEnvVarW(L"OneDriveCommercial");

    const std::wstring cacheKey =
        userProfile + L"|" + localAppData + L"|" + oneDriveEnv + L"|" +
        oneDriveConsumer + L"|" + oneDriveCommercial;

    static std::mutex s_detectCacheMutex;
    static std::wstring s_cachedKey;
    static std::vector<DetectedSyncFolder> s_cachedFolders;
    static ULONGLONG s_cachedTickMs = 0;
    constexpr ULONGLONG kDetectCacheTtlMs = 600'000ull;  // 10 分钟内复用探测结果，避免打开设置页重复触碰磁盘

    const ULONGLONG nowTick = GetTickCount64();
    {
        std::lock_guard<std::mutex> lock(s_detectCacheMutex);
        if (s_cachedTickMs != 0 &&
            s_cachedKey == cacheKey &&
            nowTick >= s_cachedTickMs &&
            (nowTick - s_cachedTickMs) < kDetectCacheTtlMs) {
            return s_cachedFolders;
        }
    }

    std::vector<DetectedSyncFolder> out;
    std::unordered_set<std::string> seenPaths;
    std::error_code ec;

    auto tryAdd = [&](const std::string& id, const std::string& label, const std::filesystem::path& baseDir) {
        if (baseDir.empty() || !std::filesystem::is_directory(baseDir, ec)) return;
        const std::filesystem::path target = baseDir / L"Tools3000Backups";
        std::string utf8 = WinUtils::wstringToUtf8(target.wstring());
        std::string lower = utf8;
        std::transform(lower.begin(), lower.end(), lower.begin(), [](unsigned char c) {
            return static_cast<char>(std::tolower(c));
        });
        if (seenPaths.insert(lower).second) {
            out.push_back({id, label, std::move(utf8)});
        }
    };

    const std::filesystem::path home(userProfile);

    // 1. Microsoft OneDrive
    for (const auto& val : {oneDriveEnv, oneDriveConsumer, oneDriveCommercial}) {
        if (!val.empty()) {
            tryAdd("onedrive", "Microsoft OneDrive", std::filesystem::path(val));
        }
    }
    if (!home.empty()) {
        tryAdd("onedrive", "Microsoft OneDrive", home / L"OneDrive");
    }

    // 2. Google Drive（桌面客户端虚拟盘或个人目录）
    if (!home.empty()) {
        tryAdd("google-drive", "Google Drive", home / L"Google Drive");
        tryAdd("google-drive", "Google Drive", home / L"My Drive");
    }
    // 仅在本机存在 Google Drive for Desktop 元数据目录时才探测 G:..Z: 挂载盘，
    // 防止未安装 Google Drive 的用户在打开设置页时唤醒休眠中的机械硬盘或触发断连盘符卡顿。
    const bool hasGoogleDriveFs =
        !localAppData.empty() &&
        std::filesystem::is_directory(std::filesystem::path(localAppData) / L"Google" / L"DriveFS", ec);
    if (hasGoogleDriveFs) {
        const DWORD logicalMask = GetLogicalDrives();
        for (wchar_t drive = L'G'; drive <= L'Z'; ++drive) {
            if ((logicalMask & (1u << (drive - L'A'))) == 0) continue;
            const std::wstring root = std::wstring(1, drive) + L":\\";
            if (GetDriveTypeW(root.c_str()) != DRIVE_FIXED) continue;
            for (const wchar_t* sub : {L"My Drive", L"我的云端硬盘", L"我的雲端硬碟"}) {
                const std::filesystem::path p = std::filesystem::path(root) / sub;
                if (std::filesystem::is_directory(p, ec)) {
                    tryAdd("google-drive", "Google Drive", p);
                    break;
                }
            }
        }
    }

    // 3. Dropbox / iCloud Drive / 坚果云
    if (!home.empty()) {
        tryAdd("dropbox", "Dropbox", home / L"Dropbox");
        tryAdd("icloud", "iCloud Drive", home / L"iCloudDrive");
        tryAdd("nutstore", "Nutstore", home / L"Nutstore");
    }

    {
        std::lock_guard<std::mutex> lock(s_detectCacheMutex);
        s_cachedKey = cacheKey;
        s_cachedFolders = out;
        s_cachedTickMs = GetTickCount64();
    }
    return out;
}

nlohmann::json ConfigBackupService::buildSnapshotEnvelope(
    const nlohmann::json& fullConfigJson,
    const std::filesystem::path& dataDir) {
    nlohmann::json sanitizedConfig = fullConfigJson.is_object() ? fullConfigJson : nlohmann::json::object();
    // 严格剔除云备份自身凭据子树与临时更新状态，防止凭据循环上传云端
    sanitizedConfig.erase("backup");
    sanitizedConfig.erase("update");

    nlohmann::json customScripts = nlohmann::json::object();
    std::error_code ec;
    if (!dataDir.empty()) {
        const std::filesystem::path scriptsDir = dataDir / L"gestures" / L"scripts";
        if (std::filesystem::is_directory(scriptsDir, ec)) {
            for (const auto& entry : std::filesystem::directory_iterator(scriptsDir, ec)) {
                if (ec || !entry.is_regular_file(ec)) continue;
                const std::wstring ext = entry.path().extension().wstring();
                if (_wcsicmp(ext.c_str(), L".lua") != 0) continue;
                const auto fsz = entry.file_size(ec);
                if (ec || fsz > 256u * 1024u) continue;
                std::ifstream ifs(entry.path(), std::ios::binary);
                if (!ifs) continue;
                std::string scriptText(
                    (std::istreambuf_iterator<char>(ifs)),
                    std::istreambuf_iterator<char>());
                const std::string name = WinUtils::wstringToUtf8(entry.path().filename().wstring());
                if (isSafeBackupFileName(name)) {
                    customScripts[name] = std::move(scriptText);
                }
            }
        }
    }

    const nlohmann::json payload = {
        {"config", std::move(sanitizedConfig)},
        {"customGestureScripts", std::move(customScripts)},
    };
    const std::string canonicalPayload = payload.dump();
    const std::string checksum = BackupCrypto::sha256Hex(canonicalPayload);

    return nlohmann::json{
        {"format", "tools3000-config-backup"},
        {"schemaVersion", kBackupSchemaVersion},
        {"encrypted", false},
        {"appVersion", tools3000::version::String},
        {"exportedAt", currentUtcIsoString()},
        {"machineName", getLocalMachineNameUtf8()},
        {"sha256", checksum},
        {"payload", payload},
    };
}

bool ConfigBackupService::verifyAndExtractSnapshot(
    const std::string& rawOrEncryptedBytes,
    const std::string& passphraseUtf8,
    nlohmann::json& outConfigJson,
    nlohmann::json& outCustomScriptsJson,
    std::string& outError) {
    outConfigJson = nlohmann::json::object();
    outCustomScriptsJson = nlohmann::json::object();
    outError.clear();

    nlohmann::json outer;
    try {
        outer = nlohmann::json::parse(rawOrEncryptedBytes);
    } catch (...) {
        outError = "invalid_backup_json";
        return false;
    }
    if (!outer.is_object()) {
        outError = "invalid_backup_root";
        return false;
    }

    nlohmann::json plainDoc;
    if (outer.value("encrypted", false)) {
        std::string plainText;
        if (!BackupCrypto::decryptEnvelope(rawOrEncryptedBytes, passphraseUtf8, plainText, outError)) {
            return false;
        }
        try {
            plainDoc = nlohmann::json::parse(plainText);
        } catch (...) {
            outError = "invalid_decrypted_payload";
            return false;
        }
    } else {
        plainDoc = std::move(outer);
    }

    if (!plainDoc.is_object() ||
        plainDoc.value("format", "") != "tools3000-config-backup" ||
        plainDoc.value("schemaVersion", 0) != kBackupSchemaVersion ||
        !plainDoc.contains("payload") || !plainDoc["payload"].is_object()) {
        outError = "unsupported_backup_schema";
        return false;
    }

    const std::string expectedSha = plainDoc.value("sha256", "");
    const std::string actualSha = BackupCrypto::sha256Hex(plainDoc["payload"].dump());
    if (expectedSha.empty() || expectedSha != actualSha) {
        outError = "checksum_mismatch";
        return false;
    }

    const auto& payload = plainDoc["payload"];
    if (!payload.contains("config") || !payload["config"].is_object()) {
        outError = "missing_config_payload";
        return false;
    }
    outConfigJson = payload["config"];
    outConfigJson.erase("backup");
    if (payload.contains("customGestureScripts") && payload["customGestureScripts"].is_object()) {
        outCustomScriptsJson = payload["customGestureScripts"];
    }
    return true;
}

BackupOperationResult ConfigBackupService::testConnection(const nlohmann::json* draftOverride) const {
    BackupOperationResult res{};
    const ResolvedBackupConfig resolved = resolveRuntimeConfig(draftOverride);
    std::string err;
    auto transport = createBackupTransport(resolved, err);
    if (!transport) {
        res.errorCode = err;
        res.message = err;
        return res;
    }
    std::string detail;
    if (!transport->testConnection(err, detail)) {
        res.errorCode = err;
        res.message = err;
        return res;
    }
    res.success = true;
    res.message = detail;
    return res;
}

BackupOperationResult ConfigBackupService::backupWithConfig(
    const ResolvedBackupConfig& resolved,
    const nlohmann::json& fullConfigJson,
    const std::filesystem::path& dataDir) {
    BackupOperationResult res{};
    std::string err;
    auto transport = createBackupTransport(resolved, err);
    if (!transport) {
        res.errorCode = err;
        res.message = err;
        return res;
    }

    const nlohmann::json snapshotDoc = buildSnapshotEnvelope(fullConfigJson, dataDir);
    std::string wirePayload = snapshotDoc.dump(2);

    if (resolved.settings.encryptionEnabled) {
        if (resolved.encryptionPassphrase.empty()) {
            res.errorCode = "empty_passphrase";
            res.message = "Encryption is enabled but passphrase is empty";
            return res;
        }
        std::string encryptedEnv;
        if (!BackupCrypto::encryptEnvelope(wirePayload, resolved.encryptionPassphrase, encryptedEnv, err)) {
            res.errorCode = err;
            res.message = err;
            return res;
        }
        wirePayload = std::move(encryptedEnv);
    }

    const char* fileExt = resolved.settings.encryptionEnabled
        ? kEncryptedBackupFileExtension
        : kBackupFileExtension;
    const std::string fileName =
        std::string(kBackupFilePrefix) + currentLocalStampCompact() + fileExt;
    if (!transport->uploadSnapshot(fileName, wirePayload, err)) {
        res.errorCode = err;
        res.message = err;
        return res;
    }

    // 保留最近 keepCount 份，自动清理超额历史快照
    const int keepCount = std::clamp(resolved.settings.keepCount, kMinKeepCount, kMaxKeepCount);
    std::vector<BackupSnapshotEntry> listed;
    std::string listErr;
    if (transport->listSnapshots(listed, listErr) && static_cast<int>(listed.size()) > keepCount) {
        for (size_t i = static_cast<size_t>(keepCount); i < listed.size(); ++i) {
            std::string delErr;
            if (transport->deleteSnapshot(listed[i].id, delErr)) {
                ++res.prunedCount;
            }
        }
    }

    res.success = true;
    res.snapshotName = fileName;
    res.bytesTransferred = static_cast<uint64_t>(wirePayload.size());
    res.message = "ok";
    return res;
}

BackupOperationResult ConfigBackupService::listWithConfig(const ResolvedBackupConfig& resolved) {
    BackupOperationResult res{};
    std::string err;
    auto transport = createBackupTransport(resolved, err);
    if (!transport) {
        res.errorCode = err;
        res.message = err;
        return res;
    }
    if (!transport->listSnapshots(res.snapshots, err)) {
        res.errorCode = err;
        res.message = err;
        return res;
    }
    res.success = true;
    return res;
}

BackupOperationResult ConfigBackupService::restoreWithConfig(
    const ResolvedBackupConfig& resolved,
    const std::string& snapshotId,
    const std::string& passphraseOverride,
    const std::filesystem::path& targetConfigFile,
    const std::filesystem::path& dataDir,
    const nlohmann::json& currentLocalBackupSubtree) {
    BackupOperationResult res{};
    std::string err;
    auto transport = createBackupTransport(resolved, err);
    if (!transport) {
        res.errorCode = err;
        res.message = err;
        return res;
    }

    std::string wireBytes;
    if (!transport->downloadSnapshot(snapshotId, wireBytes, err)) {
        res.errorCode = err;
        res.message = err;
        return res;
    }

    const std::string effectivePassphrase =
        !passphraseOverride.empty() ? passphraseOverride : resolved.encryptionPassphrase;
    nlohmann::json restoredConfig;
    nlohmann::json customScripts;
    if (!verifyAndExtractSnapshot(wireBytes, effectivePassphrase, restoredConfig, customScripts, err)) {
        res.errorCode = err;
        res.message = err;
        return res;
    }

    // 恢复前强制留存本机当前配置保险快照（防误恢复炸机）
    std::error_code ec;
    if (std::filesystem::is_regular_file(targetConfigFile, ec)) {
        const std::filesystem::path preRestore =
            targetConfigFile.parent_path() /
            WinUtils::utf8ToWstring(std::string(kPreRestoreFilePrefix) + currentLocalStampCompact() + ".json");
        std::filesystem::copy_file(
            targetConfigFile, preRestore, std::filesystem::copy_options::overwrite_existing, ec);
        if (!ec) {
            res.preRestoreBackupPath = WinUtils::wstringToUtf8(preRestore.wstring());
        }
    }

    // 保留本机云备份配置子树，防止恢复旧快照后把刚配好的云端凭据抹掉
    if (currentLocalBackupSubtree.is_object() && !currentLocalBackupSubtree.empty()) {
        restoredConfig["backup"] = currentLocalBackupSubtree;
    }

    if (!atomicWriteUtf8File(targetConfigFile, restoredConfig.dump(2))) {
        res.errorCode = "write_config_failed";
        res.message = "Failed to write restored config.json";
        return res;
    }

    // 还原自定义 Lua 手势脚本
    if (customScripts.is_object() && !customScripts.empty() && !dataDir.empty()) {
        const std::filesystem::path scriptsDir = dataDir / L"gestures" / L"scripts";
        std::filesystem::create_directories(scriptsDir, ec);
        for (auto it = customScripts.begin(); it != customScripts.end(); ++it) {
            if (!it.value().is_string() || !isSafeBackupFileName(it.key())) continue;
            atomicWriteUtf8File(
                scriptsDir / WinUtils::utf8ToWstring(it.key()),
                it.value().get<std::string>());
        }
    }

    res.success = true;
    res.snapshotName = snapshotId;
    res.bytesTransferred = static_cast<uint64_t>(wireBytes.size());
    res.message = "restored";
    return res;
}

BackupOperationResult ConfigBackupService::backupNow() {
    std::lock_guard<std::mutex> lock(m_opMutex);
    auto& cfg = ConfigManager::instance();
    const ResolvedBackupConfig resolved = resolveRuntimeConfig(nullptr);
    nlohmann::json fullCfg = nlohmann::json::object();
    try {
        fullCfg = nlohmann::json::parse(cfg.toJsonString(0));
    } catch (...) {
        fullCfg = nlohmann::json::object();
    }
    const auto dataDir = WinUtils::getAppDataDirectory();

    BackupOperationResult res = backupWithConfig(resolved, fullCfg, dataDir);
    cfg.set(kCfgLastAt, currentUtcIsoString());
    cfg.set(kCfgLastStatus, res.success ? std::string("ok") : res.errorCode);
    if (res.success && !res.snapshotName.empty()) {
        cfg.set(kCfgLastFile, res.snapshotName);
    }
    cfg.flush();

    if (res.success) {
        LOG_INFO("ConfigBackupService: backup succeeded (file={}, bytes={}, pruned={})",
                 res.snapshotName, res.bytesTransferred, res.prunedCount);
    } else {
        LOG_WARN("ConfigBackupService: backup failed (error={})", res.errorCode);
    }
    return res;
}

BackupOperationResult ConfigBackupService::listSnapshots(const nlohmann::json* draftOverride) const {
    const ResolvedBackupConfig resolved = resolveRuntimeConfig(draftOverride);
    return listWithConfig(resolved);
}

BackupOperationResult ConfigBackupService::restoreSnapshot(
    const std::string& snapshotId,
    const std::string& passphraseOverride) {
    std::lock_guard<std::mutex> lock(m_opMutex);
    auto& cfg = ConfigManager::instance();
    const ResolvedBackupConfig resolved = resolveRuntimeConfig(nullptr);
    nlohmann::json fullCfg = nlohmann::json::object();
    try {
        fullCfg = nlohmann::json::parse(cfg.toJsonString(0));
    } catch (...) {
        fullCfg = nlohmann::json::object();
    }
    const nlohmann::json localBackupSubtree =
        (fullCfg.is_object() && fullCfg.contains("backup") && fullCfg["backup"].is_object())
            ? fullCfg["backup"]
            : nlohmann::json::object();

    const std::filesystem::path dataDir = WinUtils::getAppDataDirectory();
    const std::filesystem::path configPath = dataDir / L"config" / L"config.json";
    BackupOperationResult res = restoreWithConfig(
        resolved, snapshotId, passphraseOverride, configPath, dataDir, localBackupSubtree);
    if (res.success) {
        std::ifstream ifs(configPath, std::ios::binary);
        if (ifs) {
            std::string restoredJson(
                (std::istreambuf_iterator<char>(ifs)),
                std::istreambuf_iterator<char>());
            if (!restoredJson.empty()) {
                cfg.fromJsonString(restoredJson);
                cfg.flush();
            }
        }
        LOG_INFO("ConfigBackupService: restored snapshot {} (preRestore={})",
                 snapshotId, res.preRestoreBackupPath);
    } else {
        LOG_WARN("ConfigBackupService: restore snapshot {} failed (error={})", snapshotId, res.errorCode);
    }
    return res;
}

void ConfigBackupService::runAutoBackupOnExitIfEnabled() {
    const auto s = getSettings();
    if (!s.enabled || !s.autoBackupOnExit) return;
    if (s.provider == BackupProviderType::Folder && s.folderPath.empty()) return;
    if (s.provider == BackupProviderType::WebDav && s.webdavUrl.empty()) return;
    if (s.provider == BackupProviderType::S3 && (s.s3Endpoint.empty() || s.s3Bucket.empty())) return;
    (void)backupNow();
}

nlohmann::json ConfigBackupService::settingsToJson(const CloudBackupSettings& s) {
    return nlohmann::json{
        {"enabled", s.enabled},
        {"provider", providerToString(s.provider)},
        {"autoBackupOnExit", s.autoBackupOnExit},
        {"keepCount", s.keepCount},
        {"encryptionEnabled", s.encryptionEnabled},
        {"folderPath", s.folderPath},
        {"webdavUrl", s.webdavUrl},
        {"webdavUsername", s.webdavUsername},
        {"hasWebdavPassword", s.hasWebdavPassword},
        {"s3Endpoint", s.s3Endpoint},
        {"s3Region", s.s3Region},
        {"s3Bucket", s.s3Bucket},
        {"s3Prefix", s.s3Prefix},
        {"s3AccessKeyId", s.s3AccessKeyId},
        {"s3ForcePathStyle", s.s3ForcePathStyle},
        {"hasS3SecretKey", s.hasS3SecretKey},
        {"hasEncryptionPassphrase", s.hasEncryptionPassphrase},
        {"lastBackupAtIso", s.lastBackupAtIso},
        {"lastBackupStatus", s.lastBackupStatus},
        {"lastBackupFile", s.lastBackupFile},
    };
}

nlohmann::json ConfigBackupService::resultToJson(const BackupOperationResult& r) {
    nlohmann::json list = nlohmann::json::array();
    for (const auto& item : r.snapshots) {
        list.push_back({
            {"id", item.id},
            {"fileName", item.fileName},
            {"sizeBytes", item.sizeBytes},
            {"modifiedAtIso", item.modifiedAtIso},
            {"encrypted", item.encrypted},
        });
    }
    return nlohmann::json{
        {"success", r.success},
        {"errorCode", r.errorCode},
        {"message", r.message},
        {"snapshotName", r.snapshotName},
        {"preRestoreBackupPath", r.preRestoreBackupPath},
        {"bytesTransferred", r.bytesTransferred},
        {"prunedCount", r.prunedCount},
        {"snapshots", std::move(list)},
    };
}

nlohmann::json ConfigBackupService::detectedFoldersToJson(const std::vector<DetectedSyncFolder>& folders) {
    nlohmann::json arr = nlohmann::json::array();
    for (const auto& f : folders) {
        arr.push_back({
            {"providerId", f.providerId},
            {"label", f.label},
            {"path", f.path},
        });
    }
    return arr;
}

}  // namespace tools3000::core::backup
