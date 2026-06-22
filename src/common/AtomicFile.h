#pragma once

#include <windows.h>

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <string>

namespace tools3000::common {

inline bool atomicWriteBinaryFileWithFlush(const std::wstring& targetPath, const void* data, std::size_t size) {
    if (targetPath.empty()) return false;
    static std::atomic_uint64_t sequence{0};
    const std::wstring tempPath = targetPath + L"." + std::to_wstring(GetCurrentProcessId()) +
                                  L"_" + std::to_wstring(GetTickCount64()) + L"_" +
                                  std::to_wstring(sequence.fetch_add(1, std::memory_order_relaxed)) + L".tmp";

    HANDLE file = CreateFileW(tempPath.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                              FILE_ATTRIBUTE_NORMAL | FILE_FLAG_WRITE_THROUGH, nullptr);
    if (file == INVALID_HANDLE_VALUE) return false;

    bool writeOk = true;
    std::size_t offset = 0;
    constexpr std::size_t WriteChunkBytes = 16u * 1024u * 1024u;
    const auto* bytes = static_cast<const uint8_t*>(data);
    while (offset < size) {
        const DWORD requested = static_cast<DWORD>((std::min)(WriteChunkBytes, size - offset));
        DWORD written = 0;
        if (!WriteFile(file, bytes ? (bytes + offset) : nullptr, requested, &written, nullptr) || written == 0) {
            writeOk = false;
            break;
        }
        offset += written;
    }
    if (writeOk && !FlushFileBuffers(file)) writeOk = false;
    if (!CloseHandle(file)) writeOk = false;

    if (!writeOk || offset != size) {
        DeleteFileW(tempPath.c_str());
        return false;
    }
    if (!MoveFileExW(tempPath.c_str(), targetPath.c_str(),
                     MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        DeleteFileW(tempPath.c_str());
        return false;
    }
    return true;
}

inline bool atomicWriteFileWithFlush(const std::wstring& targetPath, const std::string& data) {
    return atomicWriteBinaryFileWithFlush(targetPath, data.data(), data.size());
}

}  // namespace tools3000::common
