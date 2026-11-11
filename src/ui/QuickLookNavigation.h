#pragma once
// ─────────────────────────────────────────────────────────────────────────────
// QuickLookNavigation.h — QuickLook 极速预览文件步进与安全虚拟媒体 URL 辅助工具
//
// 职责:
//   1. 为同目录文件上下步进提供稳定的不区分大小写自然序解析 (resolveSiblingPreviewPath)；
//   2. 为虚拟主机媒体文件名提供标准 RFC 3986 路径段百分号编码 (encodeUriPathSegment)，
//      防止文件名包含 '#'、'?'、'%'、空格或非 ASCII 字符时破坏媒体/PDF 加载。
//
// 版权声明: Copyright (c) 2026 Yy1 (yuan278501381). All rights reserved.
// ─────────────────────────────────────────────────────────────────────────────

#include <windows.h>
#include <shlwapi.h>
#include <algorithm>
#include <cstdio>
#include <cwctype>
#include <filesystem>
#include <mutex>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

namespace tools3000::ui {

inline constexpr size_t kMaxEnumeratedSiblings = 10000;

struct QuickLookSiblingItem {
    std::wstring fullPath;
    std::wstring fileName;
    bool isDir = false;
};

inline std::string encodeUriPathSegment(std::string_view utf8Name) {
    std::string out;
    out.reserve(utf8Name.size() * 3);
    for (unsigned char ch : utf8Name) {
        const bool unreserved =
            (ch >= 'A' && ch <= 'Z') ||
            (ch >= 'a' && ch <= 'z') ||
            (ch >= '0' && ch <= '9') ||
            ch == '-' || ch == '_' || ch == '.' || ch == '~';
        if (unreserved) {
            out.push_back(static_cast<char>(ch));
        } else {
            char buf[4] = {};
            std::snprintf(buf, sizeof(buf), "%%%02X", static_cast<unsigned int>(ch));
            out.append(buf);
        }
    }
    return out;
}

inline std::wstring stripExtendedLengthPathPrefix(std::wstring_view rawPath) {
    if (rawPath.size() > 8 && _wcsnicmp(rawPath.data(), L"\\\\?\\UNC\\", 8) == 0) {
        std::wstring unc = L"\\\\";
        unc.append(rawPath.substr(8));
        return unc;
    }
    if (rawPath.size() > 4 && rawPath.substr(0, 4) == L"\\\\?\\") {
        return std::wstring(rawPath.substr(4));
    }
    return std::wstring(rawPath);
}

inline std::wstring normalizePreviewFilePath(std::wstring_view rawPath) {
    if (rawPath.empty()) return {};
    std::wstring trimmed = stripExtendedLengthPathPrefix(rawPath);
    const std::filesystem::path probe(trimmed);
    const size_t minLen = (std::max)(size_t{1}, probe.root_path().native().size());
    while (trimmed.size() > minLen && (trimmed.back() == L'\\' || trimmed.back() == L'/')) {
        trimmed.pop_back();
    }
    return trimmed;
}

inline std::vector<QuickLookSiblingItem> collectDirectorySiblings(
    const std::filesystem::path& parentDir,
    const std::wstring& ensureCurrentPath = {},
    const std::wstring& ensureCurrentName = {}) {
    std::error_code ec;
    if (parentDir.empty() || !std::filesystem::exists(parentDir, ec) ||
        !std::filesystem::is_directory(parentDir, ec)) {
        return {};
    }

    struct CachedDirectorySiblings {
        std::wstring parentLower;
        std::filesystem::file_time_type lastWriteTime{};
        bool hasWriteTime = false;
        bool complete = false;
        std::vector<QuickLookSiblingItem> items;
    };

    auto toLowerKey = [](std::wstring s) {
        for (wchar_t& ch : s) {
            ch = static_cast<wchar_t>(std::towlower(ch));
        }
        return s;
    };

    const std::wstring parentLower = toLowerKey(normalizePreviewFilePath(parentDir.wstring()));
    std::error_code timeEc;
    const auto dirWriteTime = std::filesystem::last_write_time(parentDir, timeEc);
    const bool hasDirWriteTime = !timeEc;

    static std::mutex s_cacheMutex;
    static CachedDirectorySiblings s_cache;

    if (hasDirWriteTime) {
        std::lock_guard<std::mutex> lock(s_cacheMutex);
        if (s_cache.complete && s_cache.hasWriteTime &&
            s_cache.parentLower == parentLower && s_cache.lastWriteTime == dirWriteTime &&
            !s_cache.items.empty()) {
            if (ensureCurrentName.empty()) {
                return s_cache.items;
            }
            for (const auto& item : s_cache.items) {
                if (_wcsicmp(item.fileName.c_str(), ensureCurrentName.c_str()) == 0) {
                    return s_cache.items;
                }
            }
        }
    }

    std::vector<QuickLookSiblingItem> siblings;
    bool hitCap = false;
    bool sawCurrent = ensureCurrentName.empty();
    for (const auto& entry : std::filesystem::directory_iterator(
             parentDir, std::filesystem::directory_options::skip_permission_denied, ec)) {
        if (ec) {
            ec.clear();
            continue;
        }
        const auto name = entry.path().filename().wstring();
        if (name.empty()) continue;
        const bool isDir = entry.is_directory(ec);
        ec.clear();
        if (!sawCurrent && _wcsicmp(name.c_str(), ensureCurrentName.c_str()) == 0) {
            sawCurrent = true;
        }
        siblings.push_back(QuickLookSiblingItem{
            normalizePreviewFilePath(entry.path().wstring()), name, isDir});
        if (siblings.size() >= kMaxEnumeratedSiblings) {
            hitCap = true;
            break;
        }
    }

    if (!sawCurrent && !ensureCurrentPath.empty()) {
        const std::filesystem::path currentPath(ensureCurrentPath);
        if (std::filesystem::exists(currentPath, ec)) {
            ec.clear();
            const bool curIsDir = std::filesystem::is_directory(currentPath, ec);
            ec.clear();
            siblings.push_back(QuickLookSiblingItem{ensureCurrentPath, ensureCurrentName, curIsDir});
        }
    }

    if (siblings.empty()) {
        return {};
    }

    // 与 Windows 资源管理器默认顺序对齐：文件夹在前、文件在后，组内按 StrCmpLogicalW 自然排序
    std::sort(siblings.begin(), siblings.end(), [](const QuickLookSiblingItem& a, const QuickLookSiblingItem& b) {
        if (a.isDir != b.isDir) {
            return a.isDir > b.isDir;
        }
        const int cmp = StrCmpLogicalW(a.fileName.c_str(), b.fileName.c_str());
        if (cmp != 0) {
            return cmp < 0;
        }
        return a.fullPath < b.fullPath;
    });

    if (!hitCap && hasDirWriteTime) {
        std::lock_guard<std::mutex> lock(s_cacheMutex);
        s_cache.parentLower = parentLower;
        s_cache.lastWriteTime = dirWriteTime;
        s_cache.hasWriteTime = true;
        s_cache.complete = true;
        s_cache.items = siblings;
    }

    return siblings;
}

inline void warmDirectorySiblingsCache(const std::wstring& filePath) {
    const std::wstring normalized = normalizePreviewFilePath(filePath);
    if (normalized.empty()) return;
    const std::filesystem::path p(normalized);
    const std::filesystem::path parentDir = p.parent_path();
    if (parentDir.empty()) return;
    (void)collectDirectorySiblings(parentDir, normalized, p.filename().wstring());
}

inline std::wstring resolveSiblingPreviewPath(const std::wstring& currentFilePath, int delta) {
    const std::wstring normalizedCurrent = normalizePreviewFilePath(currentFilePath);
    if (normalizedCurrent.empty() || delta == 0) {
        return {};
    }

    const std::filesystem::path currentPath(normalizedCurrent);
    const std::wstring currentName = currentPath.filename().wstring();
    const std::filesystem::path parentDir = currentPath.parent_path();
    if (currentName.empty() || parentDir.empty()) {
        return {};
    }

    const auto siblings = collectDirectorySiblings(parentDir, normalizedCurrent, currentName);
    if (siblings.empty()) {
        return {};
    }

    int currentIdx = -1;
    for (size_t i = 0; i < siblings.size(); ++i) {
        if (_wcsicmp(siblings[i].fileName.c_str(), currentName.c_str()) == 0) {
            currentIdx = static_cast<int>(i);
            break;
        }
    }

    if (currentIdx < 0) {
        return siblings.front().fullPath;
    }
    const int total = static_cast<int>(siblings.size());
    const int step = (delta > 0) ? 1 : -1;
    const int nextIdx = (currentIdx + step + total) % total;
    return siblings[static_cast<size_t>(nextIdx)].fullPath;
}

} // namespace tools3000::ui
