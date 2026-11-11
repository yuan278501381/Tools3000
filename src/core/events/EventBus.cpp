#include "EventBus.h"

namespace tools3000::core {

EventBus& EventBus::instance() {
    static EventBus bus;
    return bus;
}

std::shared_ptr<EventBus::Subscription> EventBus::detach(SubscriptionId id) {
    std::lock_guard lock(m_mutex);
    auto typeIt = m_idToType.find(id);
    if (typeIt == m_idToType.end()) return {};

    auto& subscribers = m_subscribers[typeIt->second];
    std::shared_ptr<Subscription> removed;
    subscribers.erase(
        std::remove_if(subscribers.begin(), subscribers.end(),
            [id, &removed](const std::shared_ptr<Subscription>& sub) {
                if (sub->id != id) return false;
                removed = sub;
                return true;
            }),
        subscribers.end()
    );
    if (subscribers.empty()) m_subscribers.erase(typeIt->second);
    m_idToType.erase(typeIt);
    return removed;
}

bool EventBus::retire(const std::shared_ptr<Subscription>& subscription, bool wait) {
    if (!subscription) return true;

    std::function<void(const std::any&)> releasedHandler;
    const DWORD currentTid = GetCurrentThreadId();

    std::unique_lock lock(subscription->stateMutex);
    subscription->accepting = false;

    const size_t selfCalls = static_cast<size_t>(std::count(
        subscription->activeThreadIds.begin(),
        subscription->activeThreadIds.end(),
        currentTid));

    if (wait) {
        using namespace std::chrono_literals;
        const bool finished = subscription->idle.wait_for(lock, 2000ms, [&subscription, selfCalls]() {
            return subscription->activeCalls <= selfCalls;
        });
        if (!finished) {
            // 超时仍有外部在途调用：严禁置空 subscription->handler，保留闭包存活以防止 UB/UAF
            m_hasQuarantinedSubscriptions.store(true, std::memory_order_release);
            LOG_WARN("EventBus: 取消订阅超时，可能存在未完成的在途调用, id={}", subscription->id);
            return false;
        }
        if (subscription->activeCalls == 0) {
            releasedHandler = std::move(subscription->handler);
            subscription->clearHandlerOnIdle = false;
            lock.unlock();
            return true;
        }
        // 同线程自退订：不阻塞等自身栈帧，交由最外层 publish 退出时安全释放
        subscription->clearHandlerOnIdle = true;
        return false;
    }

    if (subscription->activeCalls == 0) {
        releasedHandler = std::move(subscription->handler);
        subscription->clearHandlerOnIdle = false;
        lock.unlock();
        return true;
    }
    subscription->clearHandlerOnIdle = true;
    return false;
}

bool EventBus::hasQuarantinedSubscriptions() const noexcept {
    return m_hasQuarantinedSubscriptions.load(std::memory_order_acquire);
}

void EventBus::unsubscribe(SubscriptionId id) {
    retire(detach(id), false);

    LOG_TRACE("EventBus: 取消订阅, subId={}", id);
}

bool EventBus::unsubscribeAndWait(SubscriptionId id) {
    const bool drained = retire(detach(id), true);
    LOG_TRACE("EventBus: 同步取消订阅, subId={}", id);
    return drained;
}

bool EventBus::clearAll() {
    // Subscriber destructors can own RAII subscriptions which call back into
    // EventBus::unsubscribe(). Destroy them after releasing m_mutex.
    std::vector<std::shared_ptr<Subscription>> subscribers;
    decltype(m_idToType) idToType;
    {
        std::lock_guard lock(m_mutex);
        for (auto& [_, entries] : m_subscribers) {
            for (auto& subscription : entries) subscribers.push_back(std::move(subscription));
        }
        m_subscribers.clear();
        idToType.swap(m_idToType);
    }
    bool allDrained = true;
    for (const auto& subscription : subscribers) {
        if (!retire(subscription, true)) {
            allDrained = false;
        }
    }
    if (!allDrained) {
        m_hasQuarantinedSubscriptions.store(true, std::memory_order_release);
    }
    subscribers.clear();
    idToType.clear();
    LOG_INFO("EventBus: 清除所有订阅");
    return allDrained;
}

} // namespace tools3000::core
