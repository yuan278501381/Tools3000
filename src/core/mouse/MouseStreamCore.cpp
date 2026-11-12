// ─────────────────────────────────────────────────────────────────────────────
// MouseStreamCore.cpp — 高性能通用鼠标流核心基础设施实现
// ─────────────────────────────────────────────────────────────────────────────

#include "core/mouse/MouseStreamCore.h"
#include <algorithm>

namespace tools3000::core {

// ─────────────────────────────────────────────────────────────────────────────
// MouseStreamThrottle 实现
// ─────────────────────────────────────────────────────────────────────────────

bool MouseStreamThrottle::shouldWake(DWORD nowTick, DWORD minIntervalMs) noexcept {
    const DWORD last = m_lastWakeTick.load(std::memory_order_relaxed);
    if (last == 0 || (nowTick - last) >= minIntervalMs) {
        m_lastWakeTick.store(nowTick, std::memory_order_relaxed);
        return true;
    }
    return false;
}

bool MouseStreamThrottle::canWake(DWORD nowTick, DWORD minIntervalMs) const noexcept {
    const DWORD last = m_lastWakeTick.load(std::memory_order_relaxed);
    return (last == 0 || (nowTick - last) >= minIntervalMs);
}

void MouseStreamThrottle::reset() noexcept {
    m_lastWakeTick.store(0, std::memory_order_relaxed);
}

DWORD MouseStreamThrottle::lastWakeTick() const noexcept {
    return m_lastWakeTick.load(std::memory_order_relaxed);
}

// ─────────────────────────────────────────────────────────────────────────────
// MouseStreamCore 实现
// ─────────────────────────────────────────────────────────────────────────────

MouseStreamCore::MouseStreamCore() = default;

MouseStreamCore::~MouseStreamCore() {
    std::lock_guard lock(m_callbackMutex);
    m_wakeCallbackPtr.store(nullptr, std::memory_order_seq_cst);
    if (m_wakeCallbackHolder) {
        m_retiredWakeCallbacks.push_back(std::move(m_wakeCallbackHolder));
    }
    if (m_activeWakeCalls.load(std::memory_order_seq_cst) == 0) {
        m_retiredWakeCallbacks.clear();
    }
}

MouseStreamCore& MouseStreamCore::instance() {
    static MouseStreamCore inst;
    return inst;
}

void MouseStreamCore::refreshActiveState() noexcept {
    const bool shouldBeActive =
        m_explicitActive.load(std::memory_order_acquire) ||
        (m_subscriberCount.load(std::memory_order_acquire) > 0);
    const bool prev = m_active.exchange(shouldBeActive, std::memory_order_acq_rel);
    if (prev && !shouldBeActive) {
        m_queue.clear();
        m_throttle.reset();
    }
}

void MouseStreamCore::setActive(bool active) noexcept {
    m_explicitActive.store(active, std::memory_order_release);
    refreshActiveState();
}

bool MouseStreamCore::isActive() const noexcept {
    return m_active.load(std::memory_order_relaxed);
}

void MouseStreamCore::acquireSubscriber() noexcept {
    m_subscriberCount.fetch_add(1, std::memory_order_acq_rel);
    refreshActiveState();
}

void MouseStreamCore::releaseSubscriber() noexcept {
    uint32_t cur = m_subscriberCount.load(std::memory_order_relaxed);
    while (cur > 0 && !m_subscriberCount.compare_exchange_weak(
                          cur, cur - 1, std::memory_order_acq_rel, std::memory_order_relaxed)) {
    }
    refreshActiveState();
}

uint32_t MouseStreamCore::subscriberCount() const noexcept {
    return m_subscriberCount.load(std::memory_order_acquire);
}

void MouseStreamCore::setWakeEvent(HANDLE hEvent) noexcept {
    m_wakeEvent.store(hEvent, std::memory_order_release);
}

HANDLE MouseStreamCore::wakeEvent() const noexcept {
    return m_wakeEvent.load(std::memory_order_relaxed);
}

void MouseStreamCore::pushRawMove(int x, int y) noexcept {
    if (!m_active.load(std::memory_order_relaxed)) return;

    MouseStreamPoint pt{};
    pt.x = x;
    pt.y = y;
    pt.timestamp = GetTickCount();
    pt.flags = MOUSE_STREAM_FLAG_MOVE;
    pushPoint(pt);
}

void MouseStreamCore::pushPoint(const MouseStreamPoint& pt) noexcept {
    if (!m_active.load(std::memory_order_relaxed)) return;

    m_queue.push(pt);

    const DWORD interval = m_throttleIntervalMs.load(std::memory_order_relaxed);
    if (m_throttle.shouldWake(pt.timestamp, interval)) {
        // 1. 内核级 wait-free 事件唤醒 (0 闭包开销)
        HANDLE h = m_wakeEvent.load(std::memory_order_acquire);
        if (h) {
            SetEvent(h);
        }

        // 2. 纯原子指针无锁回调分发 (0 互斥锁，wait-free 引用计数保护杜绝 UAF)
        if (m_wakeCallbackPtr.load(std::memory_order_relaxed) != nullptr) {
            struct ActiveWakeGuard {
                std::atomic<uint32_t>& counter;
                explicit ActiveWakeGuard(std::atomic<uint32_t>& c) noexcept : counter(c) {
                    counter.fetch_add(1, std::memory_order_seq_cst);
                }
                ~ActiveWakeGuard() noexcept {
                    counter.fetch_sub(1, std::memory_order_release);
                }
            } guard{m_activeWakeCalls};

            auto* cb = m_wakeCallbackPtr.load(std::memory_order_acquire);
            if (cb && *cb) {
                try {
                    (*cb)();
                } catch (...) {
                }
            }
        }
    }
}

void MouseStreamCore::setWakeCallback(WakeCallback cb) {
    std::lock_guard lock(m_callbackMutex);
    if (m_wakeCallbackHolder) {
        m_retiredWakeCallbacks.push_back(std::move(m_wakeCallbackHolder));
    }
    if (cb) {
        m_wakeCallbackHolder = std::make_unique<WakeCallback>(std::move(cb));
        m_wakeCallbackPtr.store(m_wakeCallbackHolder.get(), std::memory_order_seq_cst);
    } else {
        m_wakeCallbackPtr.store(nullptr, std::memory_order_seq_cst);
    }
    if (m_activeWakeCalls.load(std::memory_order_seq_cst) == 0) {
        m_retiredWakeCallbacks.clear();
    }
}

void MouseStreamCore::setThrottleIntervalMs(DWORD intervalMs) noexcept {
    m_throttleIntervalMs.store(intervalMs, std::memory_order_relaxed);
}

DWORD MouseStreamCore::throttleIntervalMs() const noexcept {
    return m_throttleIntervalMs.load(std::memory_order_relaxed);
}

size_t MouseStreamCore::drainBatch(std::vector<MouseStreamPoint>& out, size_t maxCount) {
    size_t count = 0;
    out.reserve(out.size() + std::min(maxCount, m_queue.size()));
    MouseStreamPoint pt{};
    while (count < maxCount && m_queue.pop(pt)) {
        out.push_back(pt);
        ++count;
    }
    return count;
}

size_t MouseStreamCore::drainBatch(const PointVisitor& visitor, size_t maxCount) {
    if (!visitor) return 0;
    size_t count = 0;
    MouseStreamPoint pt{};
    while (count < maxCount && m_queue.pop(pt)) {
        visitor(pt);
        ++count;
    }
    return count;
}

bool MouseStreamCore::empty() const noexcept {
    return m_queue.empty();
}

size_t MouseStreamCore::pendingCount() const noexcept {
    return m_queue.size();
}

void MouseStreamCore::clear() noexcept {
    m_queue.clear();
}

void MouseStreamCore::resetThrottle() noexcept {
    m_throttle.reset();
}

} // namespace tools3000::core
