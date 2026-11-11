#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <algorithm>
#include <cstdint>
#include <cwctype>
#include <string>
#include <string_view>

namespace tools3000::common {

namespace detail {

inline std::wstring normalizeWinPathSeparators(std::wstring_view rawPath) {
    std::wstring out;
    out.reserve(rawPath.size());
    for (wchar_t ch : rawPath) {
        out.push_back(ch == L'/' ? L'\\' : ch);
    }
    return out;
}

inline size_t computeSafeRootPrefixLength(std::wstring_view normPath) noexcept {
    // Extended-length local drive path: \\?\C:\...
    if (normPath.size() >= 7 && normPath.starts_with(L"\\\\?\\") &&
        std::iswalpha(normPath[4]) && normPath[5] == L':' && normPath[6] == L'\\') {
        return 7;
    }
    // Standard local drive path: C:\...
    if (normPath.size() >= 3 && std::iswalpha(normPath[0]) &&
        normPath[1] == L':' && normPath[2] == L'\\') {
        return 3;
    }
    // UNC path: \\server\share\...
    if (normPath.size() >= 2 && normPath[0] == L'\\' && normPath[1] == L'\\') {
        const size_t firstSep = normPath.find(L'\\', 2);
        if (firstSep != std::wstring_view::npos) {
            const size_t secondSep = normPath.find(L'\\', firstSep + 1);
            return (secondSep != std::wstring_view::npos) ? (secondSep + 1) : normPath.size();
        }
        return normPath.size();
    }
    return 0;
}

inline bool isHandleReparseOrUnsafeObject(HANDLE hObject, bool allowDirectory) noexcept {
    if (!hObject || hObject == INVALID_HANDLE_VALUE) {
        return true;
    }
    if (GetFileType(hObject) != FILE_TYPE_DISK) {
        return true;
    }
    BY_HANDLE_FILE_INFORMATION info{};
    if (!GetFileInformationByHandle(hObject, &info)) {
        return true;
    }
    if ((info.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0) {
        return true;
    }
    if (!allowDirectory && (info.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0) {
        return true;
    }
    return false;
}

inline bool isExistingPathReparsePoint(const std::wstring& path, bool allowDirectory) noexcept {
    const DWORD attrs = GetFileAttributesW(path.c_str());
    if (attrs == INVALID_FILE_ATTRIBUTES) {
        const DWORD err = GetLastError();
        if (err == ERROR_FILE_NOT_FOUND || err == ERROR_PATH_NOT_FOUND) {
            return false;
        }
        // Conservative fail-closed on unexpected attribute query errors other than missing target
        return true;
    }
    if ((attrs & FILE_ATTRIBUTE_REPARSE_POINT) != 0) {
        return true;
    }

    HANDLE hCheck = CreateFileW(
        path.c_str(),
        FILE_READ_ATTRIBUTES,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        nullptr,
        OPEN_EXISTING,
        FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT,
        nullptr
    );
    if (hCheck == INVALID_HANDLE_VALUE) {
        const DWORD err = GetLastError();
        if (err == ERROR_FILE_NOT_FOUND || err == ERROR_PATH_NOT_FOUND) {
            return false;
        }
        return true;
    }
    const bool unsafe = isHandleReparseOrUnsafeObject(hCheck, allowDirectory);
    CloseHandle(hCheck);
    return unsafe;
}

} // namespace detail

/// Inspect targetPath and every ancestor directory below the volume root for
/// Junctions, Symlinks, or MountPoint reparse points. Returns true if any reparse
/// point or unsafe non-disk handle is detected.
inline bool pathContainsReparsePoint(const std::wstring& targetPath) noexcept {
    if (targetPath.empty()) {
        return true;
    }
    const std::wstring norm = detail::normalizeWinPathSeparators(targetPath);
    const size_t rootLen = detail::computeSafeRootPrefixLength(norm);

    size_t pos = rootLen;
    while (pos < norm.size()) {
        const size_t nextSep = norm.find(L'\\', pos);
        if (nextSep == std::wstring::npos) {
            break;
        }
        if (nextSep > pos) {
            const std::wstring ancestor = norm.substr(0, nextSep);
            if (detail::isExistingPathReparsePoint(ancestor, true)) {
                return true;
            }
        }
        pos = nextSep + 1;
    }

    std::wstring trimmed = norm;
    while (trimmed.size() > rootLen && !trimmed.empty() && trimmed.back() == L'\\') {
        trimmed.pop_back();
    }
    if (trimmed.size() > rootLen) {
        if (detail::isExistingPathReparsePoint(trimmed, true)) {
            return true;
        }
    }
    return false;
}

/// Safely create directory hierarchy step-by-step while verifying every component
/// with FILE_FLAG_OPEN_REPARSE_POINT so privileged writers never traverse Junctions/Symlinks.
inline bool ensureDirectorySafeNoReparse(const std::wstring& dirPath) noexcept {
    if (dirPath.empty()) {
        return false;
    }
    std::wstring norm = detail::normalizeWinPathSeparators(dirPath);
    const size_t rootLen = detail::computeSafeRootPrefixLength(norm);
    while (norm.size() > rootLen && !norm.empty() && norm.back() == L'\\') {
        norm.pop_back();
    }
    if (norm.empty()) {
        return false;
    }
    if (norm.size() <= rootLen) {
        return true;
    }

    size_t pos = rootLen;
    while (pos <= norm.size()) {
        const size_t nextSep = norm.find(L'\\', pos);
        const size_t endPos = (nextSep == std::wstring::npos) ? norm.size() : nextSep;
        if (endPos > pos) {
            const std::wstring currentDir = norm.substr(0, endPos);
            if (!CreateDirectoryW(currentDir.c_str(), nullptr)) {
                const DWORD err = GetLastError();
                if (err != ERROR_ALREADY_EXISTS) {
                    return false;
                }
            }

            HANDLE hDir = CreateFileW(
                currentDir.c_str(),
                FILE_READ_ATTRIBUTES,
                FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                nullptr,
                OPEN_EXISTING,
                FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT,
                nullptr
            );
            if (hDir == INVALID_HANDLE_VALUE) {
                return false;
            }
            BY_HANDLE_FILE_INFORMATION info{};
            const bool validDir =
                (GetFileType(hDir) == FILE_TYPE_DISK) &&
                GetFileInformationByHandle(hDir, &info) &&
                ((info.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0) &&
                ((info.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) == 0);
            CloseHandle(hDir);
            if (!validDir) {
                return false;
            }
        }
        if (nextSep == std::wstring::npos) {
            break;
        }
        pos = nextSep + 1;
    }
    return true;
}

inline bool atomicWriteBinaryFileWithFlush(const std::wstring& targetPath, std::string_view content) {
    if (targetPath.empty()) {
        return false;
    }
    if (pathContainsReparsePoint(targetPath)) {
        return false;
    }
    if (detail::isExistingPathReparsePoint(targetPath, false)) {
        return false;
    }

    const std::wstring normTarget = detail::normalizeWinPathSeparators(targetPath);
    const size_t lastSep = normTarget.rfind(L'\\');
    if (lastSep != std::wstring::npos) {
        const size_t rootLen = detail::computeSafeRootPrefixLength(normTarget);
        const std::wstring parentDir = normTarget.substr(0, (std::max)(lastSep, rootLen));
        if (!parentDir.empty() && parentDir.size() > rootLen) {
            if (!ensureDirectorySafeNoReparse(parentDir)) {
                return false;
            }
        }
    }

    const std::wstring tempPath =
        targetPath + L"." + std::to_wstring(GetCurrentProcessId()) + L"." +
        std::to_wstring(GetCurrentThreadId()) + L".tmp";

    if (detail::isExistingPathReparsePoint(tempPath, false)) {
        return false;
    }

    HANDLE hFile = CreateFileW(
        tempPath.c_str(),
        GENERIC_WRITE,
        0,
        nullptr,
        CREATE_ALWAYS,
        FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OPEN_REPARSE_POINT,
        nullptr
    );
    if (hFile == INVALID_HANDLE_VALUE) {
        return false;
    }

    BY_HANDLE_FILE_INFORMATION tempInfo{};
    const bool tempSafe =
        (GetFileType(hFile) == FILE_TYPE_DISK) &&
        GetFileInformationByHandle(hFile, &tempInfo) &&
        ((tempInfo.dwFileAttributes & (FILE_ATTRIBUTE_REPARSE_POINT | FILE_ATTRIBUTE_DIRECTORY)) == 0) &&
        (tempInfo.nNumberOfLinks <= 1);
    if (!tempSafe) {
        CloseHandle(hFile);
        return false;
    }

    constexpr size_t MaxWriteChunk = 16u * 1024u * 1024u;
    size_t offset = 0;
    while (offset < content.size()) {
        const DWORD toWrite = static_cast<DWORD>((std::min)(content.size() - offset, MaxWriteChunk));
        DWORD bytesWritten = 0;
        if (!WriteFile(hFile, content.data() + offset, toWrite, &bytesWritten, nullptr) ||
            bytesWritten != toWrite) {
            CloseHandle(hFile);
            DeleteFileW(tempPath.c_str());
            return false;
        }
        offset += bytesWritten;
    }

    if (!FlushFileBuffers(hFile)) {
        CloseHandle(hFile);
        DeleteFileW(tempPath.c_str());
        return false;
    }

    CloseHandle(hFile);

    // Re-verify target path and ancestor chain immediately prior to atomic rename.
    if (pathContainsReparsePoint(targetPath) || detail::isExistingPathReparsePoint(targetPath, false)) {
        DeleteFileW(tempPath.c_str());
        return false;
    }

    if (!MoveFileExW(
            tempPath.c_str(),
            targetPath.c_str(),
            MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        DeleteFileW(tempPath.c_str());
        return false;
    }

    return true;
}

inline bool atomicWriteBinaryFileWithFlush(const std::wstring& targetPath, const void* data, size_t size) {
    if (size > 0 && data == nullptr) {
        return false;
    }
    return atomicWriteBinaryFileWithFlush(
        targetPath,
        size > 0 ? std::string_view(static_cast<const char*>(data), size) : std::string_view{});
}

inline bool atomicWriteFileWithFlush(const std::wstring& targetPath, const std::string& content) {
    return atomicWriteBinaryFileWithFlush(targetPath, std::string_view(content));
}

} // namespace tools3000::common
