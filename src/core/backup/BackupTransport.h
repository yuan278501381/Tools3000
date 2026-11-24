#pragma once
// ─────────────────────────────────────────────────────────────────────────────
// BackupTransport.h — 配置云备份多态传输策略接口与工厂 (Strategy Pattern)
// ─────────────────────────────────────────────────────────────────────────────

#ifndef TOOLS3000_CORE_BACKUP_BACKUPTRANSPORT_H
#define TOOLS3000_CORE_BACKUP_BACKUPTRANSPORT_H

#include "core/backup/ConfigBackupTypes.h"
#include "core/utils/Export.h"

#include <memory>
#include <string>
#include <vector>

namespace tools3000::core::backup {

class TOOLS3000CORE_API IBackupTransport {
public:
    virtual ~IBackupTransport();

    virtual bool testConnection(std::string& outError, std::string& outDetail) = 0;
    virtual bool uploadSnapshot(
        const std::string& fileName,
        const std::string& payloadUtf8,
        std::string& outError) = 0;
    virtual bool listSnapshots(
        std::vector<BackupSnapshotEntry>& outEntries,
        std::string& outError) = 0;
    virtual bool downloadSnapshot(
        const std::string& snapshotId,
        std::string& outPayloadUtf8,
        std::string& outError) = 0;
    virtual bool deleteSnapshot(
        const std::string& snapshotId,
        std::string& outError) = 0;
};

/// 策略分发工厂：按 provider 创建对应 Folder / WebDAV / S3 传输实例
TOOLS3000CORE_API std::unique_ptr<IBackupTransport> createBackupTransport(
    const ResolvedBackupConfig& config,
    std::string& outError);

/// 安全备份文件名校验（严防路径穿越）
TOOLS3000CORE_API bool isSafeBackupFileName(std::string_view fileName) noexcept;

}  // namespace tools3000::core::backup

#endif  // TOOLS3000_CORE_BACKUP_BACKUPTRANSPORT_H
