#pragma once
#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

namespace tools3000::service {

// Global reusable query worker pool (`P-02`).
//
// Eliminates creating and joining ~51 transient OS threads on every keystroke
// across multi-drive name search, MFT shard scans, content candidate collection,
// and lock-free metadata stat batches. Supports nested `parallelFor` calls
// without deadlock via caller-helps work stealing.
class QueryThreadPool {
public:
    static QueryThreadPool& instance();

    QueryThreadPool(const QueryThreadPool&) = delete;
    QueryThreadPool& operator=(const QueryThreadPool&) = delete;

    // Executes `fn(0 .. taskCount - 1)` across pooled worker threads plus the
    // calling thread, blocking until all `taskCount` items finish.
    void parallelFor(size_t taskCount, const std::function<void(size_t)>& fn);

    // Cleanly stops all background worker threads before service exit.
    void shutdown() noexcept;

    size_t workerCount() const noexcept { return m_workerCount; }

private:
    struct JobBatch {
        size_t count = 0;
        std::atomic<size_t> nextIndex{0};
        std::atomic<size_t> remaining{0};
        std::function<void(size_t)> fn;
        std::mutex doneMutex;
        std::condition_variable doneCv;
    };

    QueryThreadPool();
    ~QueryThreadPool();

    void ensureStarted();
    void workerLoop();
    bool stealOneTask();
    void finishOneItem(const std::shared_ptr<JobBatch>& batch) noexcept;

    std::mutex m_mutex;
    std::condition_variable m_workCv;
    std::deque<std::shared_ptr<JobBatch>> m_batches;
    std::vector<std::thread> m_workers;
    size_t m_workerCount = 0;
    bool m_started = false;
    bool m_stopping = false;
};

}  // namespace tools3000::service
