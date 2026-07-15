#pragma once
// ─────────────────────────────────────────────────────────────────────────────
// SearchCancellation — 查询代际跟踪
//
// 用户连续打字时会连发多个查询，而只有最后一个的结果会被采用。此前后端把
// 每一个都完整跑完（内容搜索甚至会扫上万个候选文件并读盘），既拖慢最新的
// 查询，也占满管道工作线程。
//
// 这里用一个单调递增的"查询序号"标记代际：更新的查询一旦到达，更旧的查询
// 就被判定为过期，可在扫描循环里提前退出。序号由前端用墙钟毫秒生成，因此
// 跨窗口会话依然单调，不会因为重开搜索窗而回退。
//
// 未携带序号的请求传 0，永不参与取消，保持其它调用方的原有行为。
// ─────────────────────────────────────────────────────────────────────────────

#ifndef TOOLS3000_SERVICE_SEARCHCANCELLATION_H
#define TOOLS3000_SERVICE_SEARCHCANCELLATION_H

#include <atomic>
#include <cstdint>

namespace tools3000::service::query {

class QueryEpochTracker {
public:
    /// 记录一个新到达的查询序号。
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

    /// 取消当前所有进行中的查询。使代际递增并标记之前所有的 queryId 为过期。
    void cancelAll() noexcept {
        m_latest.fetch_add(1, std::memory_order_acq_rel);
    }

    void reset() noexcept { m_latest.store(0, std::memory_order_release); }

private:
    std::atomic<std::uint64_t> m_latest{0};
};

/// 进程内共享的查询代际。
inline QueryEpochTracker& sharedEpochTracker() noexcept {
    static QueryEpochTracker tracker;
    return tracker;
}

}  // namespace tools3000::service::query

#endif  // TOOLS3000_SERVICE_SEARCHCANCELLATION_H
