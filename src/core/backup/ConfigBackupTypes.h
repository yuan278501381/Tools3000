#pragma once
// ─────────────────────────────────────────────────────────────────────────────
// ConfigBackupTypes.h — 配置云备份领域模型、枚举与不可变常量 (SSOT)
// ─────────────────────────────────────────────────────────────────────────────

#ifndef TOOLS3000_CORE_BACKUP_CONFIGBACKUPTYPES_H
#define TOOLS3000_CORE_BACKUP_CONFIGBACKUPTYPES_H

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace tools3000::core::backup {

inline constexpr int kBackupSchemaVersion = 1;
inline constexpr const char* kBackupFilePrefix = "tools3000-backup-";
inline constexpr const char* kBackupFileExtension = ".t3kbak";
inline constexpr const char* kEncryptedBackupFileExtension = ".enc.t3kbak";
inline constexpr const char* kPreRestoreFilePrefix = "config.pre-restore-";
inline constexpr const char* kDpapiPrefix = "dpapi:v1:";
inline constexpr int kDefaultKeepCount = 10;
inline constexpr int kMinKeepCount = 1;
inline constexpr int kMaxKeepCount = 100;
inline constexpr uint32_t kPbkdf2Iterations = 100000;
inline constexpr uint32_t kHttpConnectTimeoutMs = 10000;
inline constexpr uint32_t kHttpRequestTimeoutMs = 20000;
inline constexpr size_t kMaxBackupPayloadBytes = 16u * 1024u * 1024u; // 16 MB 上限防异常内存膨胀

enum class BackupProviderType {
    None = 0,
    Folder = 1,
    WebDav = 2,
    S3 = 3,
};

struct CloudBackupSettings {
    bool enabled = false;
    BackupProviderType provider = BackupProviderType::Folder;
    bool autoBackupOnExit = false;
    int keepCount = kDefaultKeepCount;
    bool encryptionEnabled = false;

    // 1. 同步文件夹（OneDrive / Google Drive / Dropbox / iCloud / 任意本地目录）
    std::string folderPath;

    // 2. WebDAV（坚果云 / Nextcloud / OwnCloud / NAS / Alist）
    std::string webdavUrl;
    std::string webdavUsername;
    bool hasWebdavPassword = false;

    // 3. S3 兼容对象存储（Cloudflare R2 / AWS S3 / MinIO / 阿里云 OSS / 腾讯云 COS）
    std::string s3Endpoint;
    std::string s3Region = "auto";
    std::string s3Bucket;
    std::string s3Prefix = "tools3000-backups/";
    std::string s3AccessKeyId;
    bool s3ForcePathStyle = true;
    bool hasS3SecretKey = false;

    // 端到端加密口令是否已设置（永不向前端回传明文口令）
    bool hasEncryptionPassphrase = false;

    // 最近一次备份诊断元数据
    std::string lastBackupAtIso;
    std::string lastBackupStatus;
    std::string lastBackupFile;
};

struct CloudBackupSecretUpdates {
    bool updateWebdavPassword = false;
    std::string webdavPassword;
    bool updateS3SecretKey = false;
    std::string s3SecretKey;
    bool updateEncryptionPassphrase = false;
    std::string encryptionPassphrase;
};

struct ResolvedBackupConfig {
    CloudBackupSettings settings;
    std::string webdavPassword;
    std::string s3SecretKey;
    std::string encryptionPassphrase;
};

struct BackupSnapshotEntry {
    std::string id;           // 唯一标识符（文件名或对象 Key）
    std::string fileName;     // 展示文件名
    uint64_t sizeBytes = 0;
    std::string modifiedAtIso;
    bool encrypted = false;
};

struct DetectedSyncFolder {
    std::string providerId;   // onedrive | google-drive | dropbox | icloud | nutstore
    std::string label;        // 展示名称
    std::string path;         // 推荐备份目标子目录（如 C:\Users\...\OneDrive\Tools3000Backups）
};

struct BackupOperationResult {
    bool success = false;
    std::string errorCode;
    std::string message;
    std::string snapshotName;
    std::string preRestoreBackupPath;
    uint64_t bytesTransferred = 0;
    int prunedCount = 0;
    std::vector<BackupSnapshotEntry> snapshots;
};

}  // namespace tools3000::core::backup

#endif  // TOOLS3000_CORE_BACKUP_CONFIGBACKUPTYPES_H
