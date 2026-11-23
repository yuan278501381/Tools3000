#pragma once
// ─────────────────────────────────────────────────────────────────────────────
// ArchivePreview.h — 零膨胀压缩包目录结构直览解析器 (Tools3000Core)
//
// 支持格式:
//   - ZIP / JAR / APK / WHL / NUPKG / VSIX / XPI / EPUB (原生 EOCD + ZIP64 毫秒级直读)
//   - TAR / TAR.GZ / TGZ / GZ (原生 USTAR/PAX + 复用已链接 zlib 流式解析，防炸弹上限)
//   - 7Z / RAR / TAR.XZ / TAR.BZ2 / TAR.ZST (魔数探测 + Win11 系统原生 System32\tar.exe 安全枚举 + 信息卡降级)
// ─────────────────────────────────────────────────────────────────────────────

#ifndef TOOLS3000_CORE_UTILS_ARCHIVEPREVIEW_H
#define TOOLS3000_CORE_UTILS_ARCHIVEPREVIEW_H

#include "core/utils/Export.h"

#include <cstdint>
#include <string>
#include <vector>

namespace tools3000::core {

struct ArchiveEntryInfo {
    std::string path;            ///< UTF-8 归一化归档内路径 (使用 '/')
    bool isDirectory = false;
    uint64_t uncompressedSize = 0;
    uint64_t compressedSize = 0;
    bool encrypted = false;
    std::string modified;        ///< YYYY-MM-DD HH:MM 或空
};

struct ArchivePreviewResult {
    bool ok = false;
    std::string format;          ///< "ZIP", "TAR", "TAR.GZ", "GZ", "7Z", "RAR", etc.
    std::string detailNote;      ///< 如 "Win11 libarchive" / "Header probe" / 降级原因
    bool encrypted = false;
    bool truncated = false;
    uint64_t totalEntries = 0;
    uint64_t totalFiles = 0;
    uint64_t totalFolders = 0;
    uint64_t totalUncompressedBytes = 0;
    uint64_t archiveFileBytes = 0;
    std::vector<ArchiveEntryInfo> entries;
};

class TOOLS3000CORE_API ArchivePreview {
public:
    /// 零锁读取压缩包目录树（单次上限默认 2000 条展示项，10GB ZIP 仅读尾部 Central Directory <5ms）
    static ArchivePreviewResult inspectArchive(const std::wstring& filePath,
                                               size_t maxDisplayEntries = 2000);
};

}  // namespace tools3000::core

#endif  // TOOLS3000_CORE_UTILS_ARCHIVEPREVIEW_H
