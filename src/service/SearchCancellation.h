#pragma once
// ─────────────────────────────────────────────────────────────────────────────
// SearchCancellation — 查询代际跟踪
//
// 同时提供：
// 1. 服务端自增单调原子世代号 (`issueServerEpoch` / `isServerEpochStale`)：每次新编号
//    查询到达或显式 `cancelAll()` 时在服务端内部严格单调递增 `uint64_t` 世代，彻底免疫
//    客户端系统时钟回拨或超大伪造 `queryId` 永久锁死后续搜索的隐患 (`MED-17`)；
// 2. 兼容现有单元测试契约的 `observe` / `isStale` / `latest` / `cancelAll` / `reset` 接口。
// ─────────────────────────────────────────────────────────────────────────────

#ifndef TOOLS3000_SERVICE_SEARCHCANCELLATION_H
#define TOOLS3000_SERVICE_SEARCHCANCELLATION_H

#include <atomic>
#include <cstdint>

namespace tools3000::service::query {

class QueryEpochTracker {
public:
    /// 在服务端签发一个新的单调自增查询世代号（免疫客户端时钟回拨或超大 queryId）。
    /// @param tracked 为 false 时返回 0（未跟踪请求不参与抢占取消）。
    std::uint64_t issueServerEpoch(bool tracked = true) noexcept {
        if (!tracked) return 0;
        return m_serverEpoch.fetch_add(1, std::memory_order_acq_rel) + 1;
    }

    /// 判断服务端签发的世代号是否已被更新的查询或显式取消操作取代。
    bool isServerEpochStale(std::uint64_t serverEpoch) const noexcept {
        if (serverEpoch == 0) return false;
        return serverEpoch < m_serverEpoch.load(std::memory_order_acquire);
    }

    std::uint64_t latestServerEpoch() const noexcept {
        return m_serverEpoch.load(std::memory_order_acquire);
    }

    /// 记录一个新到达的客户端查询序号（保留供单元测试契约调用）。
    /// @return false 表示该查询在到达时就已经过期（有更新的查询先行到达）。
    bool observe(std::uint64_t queryId) noexcept {
        if (queryId == 0) return true;  // 未编号的调用方不参与取消
        std::uint64_t current = m_latest.load(std::memory_order_acquire);
        while (queryId > current) {
            if (m_latest.compare_exchange_weak(current, queryId,
                                               std::memory_order_acq_rel,
                                               std::memory_order_acquire)) {
                return true;
            }
        }
        return queryId == current;
    }

    /// 是否已有更新的查询到达，当前查询可以放弃。
    bool isStale(std::uint64_t queryId) const noexcept {
        if (queryId == 0) return false;
        return queryId < m_latest.load(std::memory_order_acquire);
    }

    std::uint64_t latest() const noexcept {
        return m_latest.load(std::memory_order_acquire);
    }

    /// 取消当前所有进行中的查询。使服务端世代与客户端代际同步递增。
    void cancelAll() noexcept {
        m_serverEpoch.fetch_add(1, std::memory_order_acq_rel);
        m_latest.fetch_add(1, std::memory_order_acq_rel);
    }

    void reset() noexcept {
        m_serverEpoch.store(0, std::memory_order_release);
        m_latest.store(0, std::memory_order_release);
    }

private:
    std::atomic<std::uint64_t> m_serverEpoch{0};
    std::atomic<std::uint64_t> m_latest{0};
};

/// 进程内共享的查询代际。
inline QueryEpochTracker& sharedEpochTracker() noexcept {
    static QueryEpochTracker tracker;
    return tracker;
}

}  // namespace tools3000::service::query

#endif  // TOOLS3000_SERVICE_SEARCHCANCELLATION_H
