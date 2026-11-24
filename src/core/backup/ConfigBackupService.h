#pragma once
// ─────────────────────────────────────────────────────────────────────────────
// ConfigBackupService.h — 配置云备份统一领域门面服务
//
// 支持三大策略后端:
//   1. 本地同步盘目录（自动探测 OneDrive / Google Drive / Dropbox / iCloud / 坚果云）；
//   2. 标准 WebDAV（坚果云 / Nextcloud / OwnCloud / NAS / Alist）；
//   3. 标准 S3 兼容协议（AWS SigV4：Cloudflare R2 / AWS S3 / MinIO / 阿里云 OSS）。
// 安全保障:
//   - 凭据 100% Windows DPAPI 当前用户加密存储，绝不明文落盘；
//   - 导出快照强制剔除 /backup 凭据子树，支持可选 AES-256-GCM 端到端加密；
//   - 恢复前强制留存本机 config.pre-restore-<ts>.json 防炸机保险备份。
// ─────────────────────────────────────────────────────────────────────────────

#ifndef TOOLS3000_CORE_BACKUP_CONFIGBACKUPSERVICE_H
#define TOOLS3000_CORE_BACKUP_CONFIGBACKUPSERVICE_H

#include "core/backup/ConfigBackupTypes.h"
#include "core/utils/Export.h"

#include <nlohmann/json.hpp>

#include <filesystem>
#include <mutex>
#include <string>
#include <vector>

namespace tools3000::core::backup {

class TOOLS3000CORE_API ConfigBackupService final {
public:
    static ConfigBackupService& instance();

    static std::string providerToString(BackupProviderType provider);
    static BackupProviderType providerFromString(std::string_view value);

    CloudBackupSettings getSettings() const;
    bool saveSettings(const CloudBackupSettings& next, const CloudBackupSecretUpdates& secrets);

    /// 解析当前有效运行配置（含 DPAPI 解密后的内存凭据），并可按前端草稿增量覆盖用于实时「测试连接」
    ResolvedBackupConfig resolveRuntimeConfig(const nlohmann::json* draftOverride = nullptr) const;

    /// 自动探测本机已挂载的 OneDrive / Google Drive / Dropbox / iCloud / 坚果云同步目录
    static std::vector<DetectedSyncFolder> detectLocalSyncFolders();

    /// 构建版本化配置快照 JSON（自动剔除 /backup 凭据子树，附带 Lua 自定义脚本与 SHA-256 校验）
    static nlohmann::json buildSnapshotEnvelope(
        const nlohmann::json& fullConfigJson,
        const std::filesystem::path& dataDir);

    /// 校验快照完整性并提取纯净配置 JSON 与自定义脚本集合
    static bool verifyAndExtractSnapshot(
        const std::string& rawOrEncryptedBytes,
        const std::string& passphraseUtf8,
        nlohmann::json& outConfigJson,
        nlohmann::json& outCustomScriptsJson,
        std::string& outError);

    BackupOperationResult testConnection(const nlohmann::json* draftOverride = nullptr) const;
    BackupOperationResult backupNow();
    BackupOperationResult listSnapshots(const nlohmann::json* draftOverride = nullptr) const;
    BackupOperationResult restoreSnapshot(
        const std::string& snapshotId,
        const std::string& passphraseOverride = {});

    /// 显式参数化端到端操作入口（供隔离目录单元测试与自动实机回归使用）
    static BackupOperationResult backupWithConfig(
        const ResolvedBackupConfig& resolved,
        const nlohmann::json& fullConfigJson,
        const std::filesystem::path& dataDir);

    static BackupOperationResult listWithConfig(const ResolvedBackupConfig& resolved);

    static BackupOperationResult restoreWithConfig(
        const ResolvedBackupConfig& resolved,
        const std::string& snapshotId,
        const std::string& passphraseOverride,
        const std::filesystem::path& targetConfigFile,
        const std::filesystem::path& dataDir,
        const nlohmann::json& currentLocalBackupSubtree);

    /// 程序退出冷路径调用：仅当用户主动开启 enabled + autoBackupOnExit 时静默快速备份一次
    void runAutoBackupOnExitIfEnabled();

    /// JSON 序列化辅助（供 MessageBridge IPC 直接复用）
    static nlohmann::json settingsToJson(const CloudBackupSettings& s);
    static nlohmann::json resultToJson(const BackupOperationResult& r);
    static nlohmann::json detectedFoldersToJson(const std::vector<DetectedSyncFolder>& folders);

private:
    ConfigBackupService() = default;
    ~ConfigBackupService() = default;
    ConfigBackupService(const ConfigBackupService&) = delete;
    ConfigBackupService& operator=(const ConfigBackupService&) = delete;

    mutable std::mutex m_opMutex;
};

}  // namespace tools3000::core::backup

#endif  // TOOLS3000_CORE_BACKUP_CONFIGBACKUPSERVICE_H
