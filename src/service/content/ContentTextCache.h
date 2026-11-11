#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <zlib.h>
#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cwctype>
#include <list>
#include <memory>
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
    static constexpr size_t MaxSingleRawTextBytes = 16ULL * 1024ULL * 1024ULL;
    static constexpr size_t MaxTotalCompressedBytes = 64ULL * 1024ULL * 1024ULL;
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
        bool isTruncated = false;
        if (!tryGetWithTruncation(fullPath, fileSize, lastWriteTime, outText, isTruncated)) {
            return false;
        }
        if (isTruncated) {
            outText.clear();
            return false;
        }
        return true;
    }

    bool tryGetWithTruncation(
        const std::wstring& fullPath,
        uint64_t fileSize,
        uint64_t lastWriteTime,
        std::wstring& outText,
        bool& outIsTruncated
    ) {
        outIsTruncated = false;
        const std::wstring key = normalizePathKey(fullPath);
        if (key.empty()) return false;

        std::shared_ptr<const std::vector<uint8_t>> compressedRef;
        size_t rawCharCount = 0;
        bool truncatedFlag = false;
        {
            std::lock_guard lock(m_mutex);
            const auto it = m_index.find(key);
            if (it == m_index.end()) {
                m_missCount.fetch_add(1, std::memory_order_relaxed);
                return false;
            }

            const Entry& entry = *(it->second);
            const size_t entryCompSize = entry.compressedBytes ? entry.compressedBytes->size() : 0;
            if (entry.fileSize != fileSize || entry.lastWriteTime != lastWriteTime) {
                m_totalCompressedBytes -= entryCompSize;
                m_lru.erase(it->second);
                m_index.erase(it);
                m_missCount.fetch_add(1, std::memory_order_relaxed);
                return false;
            }

            m_lru.splice(m_lru.begin(), m_lru, it->second);
            compressedRef = entry.compressedBytes;
            rawCharCount = entry.rawCharCount;
            truncatedFlag = entry.isTruncated;
        }

        if (rawCharCount == 0) {
            outText.clear();
            outIsTruncated = truncatedFlag;
            m_hitCount.fetch_add(1, std::memory_order_relaxed);
            return true;
        }
        if (!compressedRef || compressedRef->empty()) {
            m_missCount.fetch_add(1, std::memory_order_relaxed);
            return false;
        }

        std::wstring restored(rawCharCount, L'\0');
        uLongf destBytes = static_cast<uLongf>(rawCharCount * sizeof(wchar_t));
        const int zrc = uncompress(
            reinterpret_cast<Bytef*>(restored.data()),
            &destBytes,
            compressedRef->data(),
            static_cast<uLong>(compressedRef->size())
        );
        if (zrc != Z_OK || destBytes != rawCharCount * sizeof(wchar_t)) {
            std::lock_guard lock(m_mutex);
            const auto it = m_index.find(key);
            if (it != m_index.end()) {
                const size_t curCompSize = it->second->compressedBytes ? it->second->compressedBytes->size() : 0;
                m_totalCompressedBytes -= curCompSize;
                m_lru.erase(it->second);
                m_index.erase(it);
            }
            m_missCount.fetch_add(1, std::memory_order_relaxed);
            return false;
        }

        outText = std::move(restored);
        outIsTruncated = truncatedFlag;
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

        bool isTruncated = false;
        std::wstring_view sliceToStore = rawText;
        const size_t maxChars = MaxSingleRawTextBytes / sizeof(wchar_t);
        if (sliceToStore.size() > maxChars) {
            size_t cut = maxChars;
            if (cut > 0 && sliceToStore[cut - 1] >= 0xD800 && sliceToStore[cut - 1] <= 0xDBFF) {
                --cut;
            }
            sliceToStore = sliceToStore.substr(0, cut);
            isTruncated = true;
        }

        std::shared_ptr<const std::vector<uint8_t>> sharedCompressed;
        size_t newCompBytes = 0;
        if (!sliceToStore.empty()) {
            const size_t rawBytes = sliceToStore.size() * sizeof(wchar_t);
            uLongf bound = compressBound(static_cast<uLong>(rawBytes));
            std::vector<uint8_t> compressed(bound);
            const int zrc = compress2(
                compressed.data(),
                &bound,
                reinterpret_cast<const Bytef*>(sliceToStore.data()),
                static_cast<uLong>(rawBytes),
                Z_BEST_SPEED
            );
            if (zrc != Z_OK || bound == 0 || bound > MaxTotalCompressedBytes) {
                return;
            }
            compressed.resize(bound);
            compressed.shrink_to_fit();
            newCompBytes = compressed.size();
            sharedCompressed = std::make_shared<const std::vector<uint8_t>>(std::move(compressed));
        }

        std::lock_guard lock(m_mutex);
        const auto existing = m_index.find(key);
        if (existing != m_index.end()) {
            const size_t oldSize = existing->second->compressedBytes ? existing->second->compressedBytes->size() : 0;
            m_totalCompressedBytes -= oldSize;
            m_lru.erase(existing->second);
            m_index.erase(existing);
        }

        while (!m_lru.empty() &&
               ((m_totalCompressedBytes + newCompBytes > MaxTotalCompressedBytes) ||
                (m_lru.size() >= MaxCachedEntries))) {
            const Entry& victim = m_lru.back();
            const size_t victimSize = victim.compressedBytes ? victim.compressedBytes->size() : 0;
            m_totalCompressedBytes -= victimSize;
            m_index.erase(victim.key);
            m_lru.pop_back();
        }

        m_totalCompressedBytes += newCompBytes;
        m_lru.push_front(Entry{
            .key = key,
            .fileSize = fileSize,
            .lastWriteTime = lastWriteTime,
            .rawCharCount = sliceToStore.size(),
            .isTruncated = isTruncated,
            .compressedBytes = std::move(sharedCompressed)
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
        bool isTruncated = false;
        std::shared_ptr<const std::vector<uint8_t>> compressedBytes;
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
