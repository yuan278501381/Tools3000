#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <zlib.h>
#include <atomic>
#include <cstdint>
#include <cwctype>
#include <list>
#include <mutex>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace tools3000::service::content {

struct FileCacheStamp {
    uint64_t fileSize = 0;
    uint64_t lastWriteTime = 0;
    bool valid = false;
};

inline FileCacheStamp queryFileCacheStampByHandle(HANDLE hFile) noexcept {
    FileCacheStamp stamp;
    if (!hFile || hFile == INVALID_HANDLE_VALUE) return stamp;

    BY_HANDLE_FILE_INFORMATION info{};
    if (!GetFileInformationByHandle(hFile, &info)) {
        return stamp;
    }

    stamp.fileSize = (static_cast<uint64_t>(info.nFileSizeHigh) << 32) |
                     static_cast<uint64_t>(info.nFileSizeLow);
    stamp.lastWriteTime = (static_cast<uint64_t>(info.ftLastWriteTime.dwHighDateTime) << 32) |
                          static_cast<uint64_t>(info.ftLastWriteTime.dwLowDateTime);
    stamp.valid = true;
    return stamp;
}

inline FileCacheStamp queryFileCacheStampByPath(const std::wstring& filePath) noexcept {
    FileCacheStamp stamp;
    WIN32_FILE_ATTRIBUTE_DATA attr{};
    if (!GetFileAttributesExW(filePath.c_str(), GetFileExInfoStandard, &attr)) {
        return stamp;
    }
    if ((attr.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0) {
        return stamp;
    }
    stamp.fileSize = (static_cast<uint64_t>(attr.nFileSizeHigh) << 32) |
                     static_cast<uint64_t>(attr.nFileSizeLow);
    stamp.lastWriteTime = (static_cast<uint64_t>(attr.ftLastWriteTime.dwHighDateTime) << 32) |
                          static_cast<uint64_t>(attr.ftLastWriteTime.dwLowDateTime);
    stamp.valid = true;
    return stamp;
}

// On-demand, strictly in-memory zlib-compressed LRU cache for parsed rich-document text.
// Zero work occurs until an explicit user content search parses a rich document.
class ContentTextCache {
public:
    static constexpr size_t MaxSingleRawTextBytes = 4ULL * 1024ULL * 1024ULL;
    static constexpr size_t MaxTotalCompressedBytes = 48ULL * 1024ULL * 1024ULL;
    static constexpr size_t MaxCachedEntries = 16384;

    static ContentTextCache& instance() {
        static ContentTextCache cache;
        return cache;
    }

    bool tryGet(
        const std::wstring& fullPath,
        uint64_t fileSize,
        uint64_t lastWriteTime,
        std::wstring& outText
    ) {
        const std::wstring key = normalizePathKey(fullPath);
        if (key.empty()) return false;

        std::vector<uint8_t> compressedCopy;
        size_t rawCharCount = 0;
        {
            std::lock_guard lock(m_mutex);
            const auto it = m_index.find(key);
            if (it == m_index.end()) {
                m_missCount.fetch_add(1, std::memory_order_relaxed);
                return false;
            }

            const Entry& entry = *(it->second);
            if (entry.fileSize != fileSize || entry.lastWriteTime != lastWriteTime) {
                m_totalCompressedBytes -= entry.compressedBytes.size();
                m_lru.erase(it->second);
                m_index.erase(it);
                m_missCount.fetch_add(1, std::memory_order_relaxed);
                return false;
            }

            m_lru.splice(m_lru.begin(), m_lru, it->second);
            compressedCopy = entry.compressedBytes;
            rawCharCount = entry.rawCharCount;
        }

        if (rawCharCount == 0) {
            outText.clear();
            m_hitCount.fetch_add(1, std::memory_order_relaxed);
            return true;
        }

        std::wstring restored(rawCharCount, L'\0');
        uLongf destBytes = static_cast<uLongf>(rawCharCount * sizeof(wchar_t));
        const int zrc = uncompress(
            reinterpret_cast<Bytef*>(restored.data()),
            &destBytes,
            compressedCopy.data(),
            static_cast<uLong>(compressedCopy.size())
        );
        if (zrc != Z_OK || destBytes != rawCharCount * sizeof(wchar_t)) {
            std::lock_guard lock(m_mutex);
            const auto it = m_index.find(key);
            if (it != m_index.end()) {
                m_totalCompressedBytes -= it->second->compressedBytes.size();
                m_lru.erase(it->second);
                m_index.erase(it);
            }
            m_missCount.fetch_add(1, std::memory_order_relaxed);
            return false;
        }

        outText = std::move(restored);
        m_hitCount.fetch_add(1, std::memory_order_relaxed);
        return true;
    }

    void put(
        const std::wstring& fullPath,
        uint64_t fileSize,
        uint64_t lastWriteTime,
        std::wstring_view rawText
    ) {
        const std::wstring key = normalizePathKey(fullPath);
        if (key.empty()) return;

        std::vector<uint8_t> compressed;
        if (!rawText.empty()) {
            const size_t rawBytes = rawText.size() * sizeof(wchar_t);
            if (rawBytes > MaxSingleRawTextBytes) {
                return;
            }

            uLongf bound = compressBound(static_cast<uLong>(rawBytes));
            compressed.resize(bound);
            const int zrc = compress2(
                compressed.data(),
                &bound,
                reinterpret_cast<const Bytef*>(rawText.data()),
                static_cast<uLong>(rawBytes),
                Z_BEST_SPEED
            );
            if (zrc != Z_OK || bound == 0 || bound > MaxTotalCompressedBytes) {
                return;
            }
            compressed.resize(bound);
            compressed.shrink_to_fit();
        }

        std::lock_guard lock(m_mutex);
        const auto existing = m_index.find(key);
        if (existing != m_index.end()) {
            m_totalCompressedBytes -= existing->second->compressedBytes.size();
            m_lru.erase(existing->second);
            m_index.erase(existing);
        }

        while (!m_lru.empty() &&
               ((m_totalCompressedBytes + compressed.size() > MaxTotalCompressedBytes) ||
                (m_lru.size() >= MaxCachedEntries))) {
            const Entry& victim = m_lru.back();
            m_totalCompressedBytes -= victim.compressedBytes.size();
            m_index.erase(victim.key);
            m_lru.pop_back();
        }

        m_totalCompressedBytes += compressed.size();
        m_lru.push_front(Entry{
            .key = key,
            .fileSize = fileSize,
            .lastWriteTime = lastWriteTime,
            .rawCharCount = rawText.size(),
            .compressedBytes = std::move(compressed)
        });
        m_index[key] = m_lru.begin();
    }

    void clear() {
        std::lock_guard lock(m_mutex);
        m_lru.clear();
        m_index.clear();
        m_totalCompressedBytes = 0;
        m_hitCount.store(0, std::memory_order_relaxed);
        m_missCount.store(0, std::memory_order_relaxed);
    }

    size_t totalCompressedBytes() const {
        std::lock_guard lock(m_mutex);
        return m_totalCompressedBytes;
    }

    size_t entryCount() const {
        std::lock_guard lock(m_mutex);
        return m_index.size();
    }

    uint64_t hitCount() const noexcept {
        return m_hitCount.load(std::memory_order_relaxed);
    }

    uint64_t missCount() const noexcept {
        return m_missCount.load(std::memory_order_relaxed);
    }

private:
    struct Entry {
        std::wstring key;
        uint64_t fileSize = 0;
        uint64_t lastWriteTime = 0;
        size_t rawCharCount = 0;
        std::vector<uint8_t> compressedBytes;
    };

    static std::wstring normalizePathKey(std::wstring_view path) {
        std::wstring norm;
        norm.reserve(path.size());
        for (wchar_t ch : path) {
            norm.push_back(ch == L'/' ? L'\\' : static_cast<wchar_t>(std::towlower(ch)));
        }
        return norm;
    }

    mutable std::mutex m_mutex;
    std::list<Entry> m_lru;
    std::unordered_map<std::wstring, typename std::list<Entry>::iterator> m_index;
    size_t m_totalCompressedBytes = 0;
    std::atomic<uint64_t> m_hitCount{0};
    std::atomic<uint64_t> m_missCount{0};
};

} // namespace tools3000::service::content
