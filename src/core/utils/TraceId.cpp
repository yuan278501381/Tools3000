#include "core/utils/TraceId.h"

#include <atomic>
#include <chrono>
#include <ctime>
#include <format>
#include <random>

namespace tools3000::core {

std::string TraceId::generate() {
    auto now = std::chrono::system_clock::now();
    auto timeT = std::chrono::system_clock::to_time_t(now);
    std::tm tm{};
    localtime_s(&tm, &timeT);

    // 4位16进制随机后缀 —— 保证同一秒内的唯一性（由 Tools3000Core.dll 全局单实例持有）
    static std::atomic<uint32_t> counter{0};
    static thread_local std::mt19937 rng{std::random_device{}()};
    uint16_t suffix = static_cast<uint16_t>(rng()) ^
                      static_cast<uint16_t>(counter.fetch_add(1, std::memory_order_relaxed));

    return std::format("{:04d}{:02d}{:02d}-{:02d}{:02d}{:02d}-{:04X}",
                       tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday,
                       tm.tm_hour, tm.tm_min, tm.tm_sec,
                       suffix);
}

const std::string& TraceId::current() {
    std::string& id = storage();
    if (id.empty()) {
        id = generate();
    }
    return id;
}

void TraceId::setCurrent(const std::string& traceId) {
    storage() = traceId;
}

std::string TraceId::begin() {
    std::string id = generate();
    storage() = id;
    return id;
}

std::string& TraceId::storage() {
    thread_local std::string currentId;
    return currentId;
}

}  // namespace tools3000::core
