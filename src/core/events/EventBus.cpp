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

void EventBus::retire(const std::shared_ptr<Subscription>& subscription, bool wait) {
    if (!subscription) return;
    std::unique_lock lock(subscription->stateMutex);
    subscription->accepting = false;
    if (wait) {
        using namespace std::chrono_literals;
        const bool finished = subscription->idle.wait_for(lock, 2000ms, [&subscription]() {
            return subscription->activeCalls == 0;
        });
        if (!finished) {
            LOG_WARN("EventBus: 取消订阅超时，可能存在未完成的在途调用, id={}", subscription->id);
        }
        subscription->handler = nullptr;
    }
}

void EventBus::unsubscribe(SubscriptionId id) {
    retire(detach(id), false);

    LOG_TRACE("EventBus: 取消订阅, subId={}", id);
}

void EventBus::unsubscribeAndWait(SubscriptionId id) {
    retire(detach(id), true);
    LOG_TRACE("EventBus: 同步取消订阅, subId={}", id);
}

void EventBus::clearAll() {
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
    for (const auto& subscription : subscribers) retire(subscription, true);
    subscribers.clear();
    idToType.clear();
    LOG_INFO("EventBus: 清除所有订阅");
}

} // namespace tools3000::core
