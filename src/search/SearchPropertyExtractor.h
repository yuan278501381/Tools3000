#pragma once

#include <atomic>
#include <cstdint>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

#include <nlohmann/json.hpp>

namespace tools3000::search {

struct PropertyExtractItem {
    std::string path;
    bool isDirectory = false;
    uint64_t fileSize = 0;
    uint64_t lastWriteTime = 0;
};

struct ExtractedPropertyValue {
    std::string display;
    std::optional<double> sortNumber;
};

struct BatchPropertyExtractResult {
    // key = 请求 item.path，value = { propId -> ExtractedPropertyValue }
    std::unordered_map<std::string, std::unordered_map<std::string, ExtractedPropertyValue>> values;
    size_t cacheHits = 0;
    size_t diskExtractions = 0;
    bool budgetExceeded = false;
    bool cancelled = false;
};

/**
 * Tier 2 高性能视口惰性属性提取引擎：
 * 面向搜索结果表格的自定义扩展列，采用按需策略桶分发 + 进程级线程安全 LRU 缓存 +
 * 按扩展名缓存的 Shell 文件类型描述 + WIC 零解码读头 + Windows Property System 通用读取 +
 * 云端脱机文件防下载保护与代际取消机制。
 */
class SearchPropertyExtractor {
public:
    static constexpr size_t kMaxBatchItems = 64;
    static constexpr size_t kMaxBatchProperties = 32;
    static constexpr uint64_t kBatchBudgetMs = 1800;
    static constexpr size_t kPropertyCacheCapacity = 4096;
    static constexpr size_t kShellTypeCacheCapacity = 2048;
    static constexpr uint64_t kMaxHashFileBytes = 64ULL * 1024ULL * 1024ULL;
    static constexpr size_t kMaxChildScanLimit = 50000;

    static SearchPropertyExtractor& instance();

    BatchPropertyExtractResult extractBatch(
        const std::vector<PropertyExtractItem>& items,
        const std::vector<std::string>& propertyIds,
        uint64_t requestGen = 0
    );

    // 取消早于指定代际的视口批次提取任务（防止快速滚屏堆积阻塞工作线程池）。
    void advanceRequestGeneration(uint64_t latestGen) noexcept;

    nlohmann::json handleBatchRequestJson(const nlohmann::json& params);

    // 格式化 Windows 文件属性位掩码为标准紧凑缩写（如 "RHSADCI"）。
    static std::string formatFileAttributes(uint32_t attributes);

    // 判断是否为 OneDrive / 云盘按需脱机占位文件（禁止读盘以防拉起云下载阻塞）。
    static bool isCloudPlaceholderAttributes(uint32_t attributes) noexcept;

    // 根据扩展名和是否目录查询系统本地化文件类型描述（内置线程安全缓存，0 磁盘 I/O）。
    std::string getShellTypeName(const std::wstring& extensionLower, bool isDirectory);

    // 仅用于单元测试或配置重置时清空内存缓存。
    void clearCache();

private:
    SearchPropertyExtractor() = default;
    ~SearchPropertyExtractor() = default;
    SearchPropertyExtractor(const SearchPropertyExtractor&) = delete;
    SearchPropertyExtractor& operator=(const SearchPropertyExtractor&) = delete;

    std::atomic<uint64_t> m_latestRequestGen{0};
};

} // namespace tools3000::search
