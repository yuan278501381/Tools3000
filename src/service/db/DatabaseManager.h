#pragma once
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <string>
#include <vector>
#include <memory>
#include <mutex>
#include <cstdint>

class MftParser;

namespace tools3000::service::db {

#pragma pack(push, 1)

struct DbHeader {
    uint64_t magic = 0x3230424459534145ULL; // 'EASYDB02'
    uint32_t version = 2;
    uint32_t volumeCount = 0;
    uint64_t timestamp = 0;
    uint64_t totalRecords = 0;
};

struct DbVolumeHeader {
    wchar_t driveLetter = L'\0';
    uint16_t reserved = 0;
    uint32_t volumeSerial = 0;
    uint64_t lastUsn = 0;
    uint32_t recordCount = 0;
    uint32_t stringPoolBytes = 0;
};

struct DbRecordPod {
    uint64_t frn = 0;
    uint64_t parentFrn = 0;
    uint32_t attributes = 0;
    uint32_t nameOffset = 0; // wchar_t offset in string pool
    uint16_t nameLen = 0;    // wchar_t count
    uint8_t isDirectory = 0;
    uint8_t reserved = 0;
    uint64_t fileSize = 0;
    uint64_t creationTime = 0;
    uint64_t lastWriteTime = 0;
};

#pragma pack(pop)

struct DbStats {
    std::wstring dbPath;
    uint64_t fileSize = 0;
    uint64_t timestamp = 0;
    uint64_t totalRecords = 0;
    uint32_t volumeCount = 0;
    bool exists = false;
};

class DatabaseManager {
public:
    static DatabaseManager& instance();

    DatabaseManager();
    ~DatabaseManager();

    // 初始化快照数据库路径 (默认 %APPDATA%\Tools3000\Tools3000.db)
    void init(const std::wstring& customDbPath = L"");

    // 保存所有卷的索引快照至 Tools3000.db
    bool saveSnapshot(const std::vector<MftParser*>& parsers);

    // 从 Tools3000.db 加载快照数据至 MftParser 卷列表
    bool loadSnapshot(std::vector<MftParser*>& parsers);

    // 获取数据库文件元数据
    DbStats getStats() const;

    std::wstring getDbPath() const;

private:
    mutable std::mutex m_mutex;
    std::wstring m_dbPath;
};

} // namespace tools3000::service::db
