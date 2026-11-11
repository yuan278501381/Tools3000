#include "core/events/MainThreadDispatcher.h"
#include "core/logger/Logger.h"

#include <atomic>
#include <deque>
#include <mutex>
#include <utility>

namespace tools3000::core {

struct MainThreadDispatcher::Impl {
    std::atomic<HWND> window{nullptr};
    std::atomic<DWORD> ownerThread{0};
    std::mutex mutex;
    std::deque<Task> tasks;
};

MainThreadDispatcher& MainThreadDispatcher::instance() {
    static MainThreadDispatcher dispatcher;
    return dispatcher;
}

void MainThreadDispatcher::initialize(HWND messageWindow) {
    Impl* impl = m_impl.load(std::memory_order_acquire);
    if (!impl) {
        auto* candidate = new Impl();
        Impl* expected = nullptr;
        if (!m_impl.compare_exchange_strong(expected, candidate, std::memory_order_acq_rel)) {
            delete candidate;
            impl = expected;
        } else {
            impl = candidate;
        }
    }
    {
        std::lock_guard lock(impl->mutex);
        impl->tasks.clear();
        impl->ownerThread.store(GetCurrentThreadId(), std::memory_order_release);
        impl->window.store(messageWindow, std::memory_order_release);
    }
}

void MainThreadDispatcher::shutdown() {
    Impl* impl = m_impl.load(std::memory_order_acquire);
    if (!impl) return;
    std::lock_guard lock(impl->mutex);
    impl->window.store(nullptr, std::memory_order_release);
    impl->ownerThread.store(0, std::memory_order_release);
    impl->tasks.clear();
}

bool MainThreadDispatcher::isOwnerThread() const {
    Impl* impl = m_impl.load(std::memory_order_acquire);
    return impl && impl->ownerThread.load(std::memory_order_acquire) == GetCurrentThreadId();
}

bool MainThreadDispatcher::isInitialized() const {
    Impl* impl = m_impl.load(std::memory_order_acquire);
    return impl && impl->ownerThread.load(std::memory_order_acquire) != 0;
}

bool MainThreadDispatcher::post(Task task) {
    if (!task) return false;
    if (isOwnerThread()) {
        task();
        return true;
    }
    return postDeferred(std::move(task));
}

bool MainThreadDispatcher::postDeferred(Task task) {
    if (!task) return false;
    Impl* impl = m_impl.load(std::memory_order_acquire);
    if (!impl) return false;

    if (!impl->window.load(std::memory_order_acquire)) return false;
    {
        std::lock_guard lock(impl->mutex);
        const HWND lockedWindow = impl->window.load(std::memory_order_relaxed);
        if (!lockedWindow) return false;
        const bool needWakeup = impl->tasks.empty();
        impl->tasks.push_back(std::move(task));
        if (needWakeup && !PostMessageW(lockedWindow, MessageId, 0, 0)) {
            impl->tasks.pop_back();
            LOG_ERROR("MainThreadDispatcher: PostMessageW failed, error={}", GetLastError());
            return false;
        }
    }
    return true;
}

void MainThreadDispatcher::drain() {
    Impl* impl = m_impl.load(std::memory_order_acquire);
    if (!impl || !isOwnerThread()) return;
    std::deque<Task> tasks;
    {
        std::lock_guard lock(impl->mutex);
        tasks.swap(impl->tasks);
    }
    for (auto& task : tasks) {
        try {
            task();
        } catch (const std::exception& e) {
            LOG_ERROR("MainThreadDispatcher task failed: {}", e.what());
        } catch (...) {
            LOG_ERROR("MainThreadDispatcher task failed with unknown exception");
        }
    }
}

}  // namespace tools3000::core
