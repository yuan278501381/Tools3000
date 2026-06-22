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
    if (!m_impl) m_impl = new Impl();
    {
        std::lock_guard lock(m_impl->mutex);
        m_impl->tasks.clear();
    }
    m_impl->ownerThread.store(GetCurrentThreadId(), std::memory_order_release);
    m_impl->window.store(messageWindow, std::memory_order_release);
}

void MainThreadDispatcher::shutdown() {
    if (!m_impl) return;
    m_impl->window.store(nullptr, std::memory_order_release);
    m_impl->ownerThread.store(0, std::memory_order_release);
    std::lock_guard lock(m_impl->mutex);
    m_impl->tasks.clear();
}

bool MainThreadDispatcher::isOwnerThread() const {
    return m_impl && m_impl->ownerThread.load(std::memory_order_acquire) == GetCurrentThreadId();
}

bool MainThreadDispatcher::isInitialized() const {
    return m_impl && m_impl->ownerThread.load(std::memory_order_acquire) != 0;
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
    if (!m_impl) return false;

    const HWND window = m_impl->window.load(std::memory_order_acquire);
    if (!window) return false;
    {
        std::lock_guard lock(m_impl->mutex);
        if (!m_impl->window.load(std::memory_order_relaxed)) return false;
        m_impl->tasks.push_back(std::move(task));
        if (!PostMessageW(window, MessageId, 0, 0)) {
            m_impl->tasks.pop_back();
            LOG_ERROR("MainThreadDispatcher: PostMessageW failed, error={}", GetLastError());
            return false;
        }
    }
    return true;
}

void MainThreadDispatcher::drain() {
    if (!m_impl || !isOwnerThread()) return;
    std::deque<Task> tasks;
    {
        std::lock_guard lock(m_impl->mutex);
        tasks.swap(m_impl->tasks);
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
