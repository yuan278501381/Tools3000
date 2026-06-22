#pragma once
// ─────────────────────────────────────────────────────────────────────────────
// EventBus — 进程内事件总线
//
// 设计目标:
//   1. 类型安全的发布/订阅模式，解耦模块间通信
//   2. 支持同步事件分发（直接回调）与异步事件分发（投递到消息队列）
//   3. 订阅 ID 管理，支持安全取消订阅
//   4. 零运行时开销的事件注册（constexpr 事件 ID）
//   5. 线程安全：所有方法可从任意线程调用
//
// 使用示例:
//   // 订阅
//   auto id = EventBus::instance().subscribe<ScreenshotCompletedEvent>(
//       [](const ScreenshotCompletedEvent& e) {
//           LOG_INFO("截图完成: {}x{}", e.width, e.height);
//       });
//
//   // 发布
//   EventBus::instance().publish(ScreenshotCompletedEvent{800, 600, "/path/to/file"});
//
//   // 取消订阅
//   EventBus::instance().unsubscribe(id);
// ─────────────────────────────────────────────────────────────────────────────

#ifndef TOOLS3000_CORE_EVENTS_EVENTBUS_H
#define TOOLS3000_CORE_EVENTS_EVENTBUS_H

#include "core/utils/Export.h"
#include "core/logger/Logger.h"

#include <any>
#include <atomic>
#include <condition_variable>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <typeindex>
#include <unordered_map>
#include <vector>

namespace tools3000::core {

/// 订阅 ID，用于取消订阅
using SubscriptionId = uint64_t;

// ─────────────────────────────────────────────────────────────────────────────
// 事件类型定义
// ─────────────────────────────────────────────────────────────────────────────

/// 截图完成事件
struct ScreenshotCompletedEvent {
    int width = 0;
    int height = 0;
    std::string filePath;
    bool copiedToClipboard = false;
};

/// 手势识别完成事件
struct GestureRecognizedEvent {
    std::string gestureCode;   // 方向编码 (如 "L-U-R")
    std::string actionName;    // 匹配到的动作名称
    std::string profileName;   // 使用的 Profile
    bool executed = false;     // 是否成功执行
};

/// 录屏状态变化事件
struct RecordingStateChangedEvent {
    int state = 0;       // 0=Idle, 1=Recording, 2=Paused
    int frameCount = 0;
    int droppedFrameCount = 0;
    double durationSec = 0.0;
    std::string outputPath;
};

/// 配置变化事件
struct ConfigChangedEvent {
    std::string key;
    std::string oldValue;
    std::string newValue;
};

/// 快捷键触发事件
struct HotkeyTriggeredEvent {
    std::string name;     // 快捷键名称
    std::string binding;  // 按键组合文字 (如 "Ctrl+Shift+A")
};

/// 手势暂停状态变化
struct GesturePauseChangedEvent {
    bool paused = false;
};

/// 贴图窗口创建事件
struct PinWindowCreatedEvent {
    int x = 0, y = 0;
    int width = 0, height = 0;
    int totalPinCount = 0;
};

/// 插件加载事件
struct PluginLoadedEvent {
    std::string pluginName;
    std::string version;
    bool success = false;
};

/// 触发截图事件
struct ActionTriggerScreenshotEvent {};

/// 触发录屏切换事件
struct ActionToggleRecordingEvent {};

/// 触发手势暂停切换事件
struct ActionToggleGesturePauseEvent {};

/// 取消正在进行的瞬时 UI（手势追踪、轮盘菜单）。Esc 发出，不吞掉按键。
struct CancelTransientUiEvent {};

/// 系统会话状态变更事件 (锁屏、注销、解锁等)
struct SystemSessionChangedEvent {
    uint32_t sessionEvent = 0;
};

/// 系统电源状态变更事件 (休眠挂起、唤醒恢复等)
struct SystemPowerChangedEvent {
    uint32_t powerEvent = 0;
};

/// 鼠标演示与特效启用状态变化事件
struct SpotlightStateChangedEvent {
    bool enabled = true;
};

// ─────────────────────────────────────────────────────────────────────────────// 显示全局 Toast 的事件
struct ShowToastEvent {
    std::wstring message;
};

/// 主题模式与强调色变更事件
struct ThemeChangedEvent {
    std::string theme;       // "system" | "light" | "dark"
    std::string accentColor; // "violet" | "cyan" | "amber" | "blue" | "mint" | "coral"
    std::string trayIconTheme = "system"; // "system" | "light" | "dark" | "color"
};

/// 鼠标活动事件 (供鼠标聚光灯、水波纹与轨迹等演示特效使用)
struct MouseActivityEvent {
    int button = -1; // 0: LeftDown, 1: RightDown, 2: MiddleDown, -1: Move
    long x = 0;
    long y = 0;
};

// =========================================================================
// EventBus 实现
// ─────────────────────────────────────────────────────────────────────────────

class TOOLS3000CORE_API EventBus {
public:
    static EventBus& instance();

    /// 订阅事件
    /// @tparam TEvent 事件类型
    /// @param handler 事件处理函数
    /// @return 订阅 ID，用于取消订阅
    template <typename TEvent>
    SubscriptionId subscribe(std::function<void(const TEvent&)> handler) {
        SubscriptionId id = m_nextId.fetch_add(1);
        auto subscription = std::make_shared<Subscription>();
        subscription->id = id;
        subscription->handler = [handler = std::move(handler)](const std::any& event) {
            try {
                handler(std::any_cast<const TEvent&>(event));
            } catch (const std::bad_any_cast& e) {
                LOG_ERROR("EventBus: 事件类型转换失败: {}", e.what());
            } catch (const std::exception& e) {
                LOG_ERROR("EventBus: 事件处理器异常: {}", e.what());
            } catch (...) {
                LOG_ERROR("EventBus: 事件处理器未知异常");
            }
        };
        {
            std::lock_guard lock(m_mutex);
            auto typeId = std::type_index(typeid(TEvent));
            m_subscribers[typeId].push_back(std::move(subscription));
            m_idToType.insert_or_assign(id, typeId);
        }

        LOG_TRACE("EventBus: 订阅事件 [{}], subId={}", typeid(TEvent).name(), id);
        return id;
    }

    /// 取消订阅
    void unsubscribe(SubscriptionId id);

    /// 取消订阅并等待正在执行的回调结束。插件卸载前必须使用此接口。
    void unsubscribeAndWait(SubscriptionId id);

    /// 发布事件（同步分发给所有订阅者）
    template <typename TEvent>
    void publish(const TEvent& event) {
        std::vector<std::shared_ptr<Subscription>> subscribersCopy;
        {
            std::lock_guard lock(m_mutex);
            auto typeId = std::type_index(typeid(TEvent));
            auto it = m_subscribers.find(typeId);
            if (it == m_subscribers.end()) return;
            subscribersCopy = it->second;  // 副本，避免持锁回调
        }

        LOG_TRACE("EventBus: 发布事件 [{}], 订阅者数={}", typeid(TEvent).name(), subscribersCopy.size());

        std::any wrappedEvent = event;
        for (const auto& sub : subscribersCopy) {
            bool acquired = false;
            {
                std::lock_guard lock(sub->stateMutex);
                if (sub->accepting && sub->handler) {
                    ++sub->activeCalls;
                    acquired = true;
                }
            }
            if (!acquired) continue;
            try {
                sub->handler(wrappedEvent);
            } catch (...) {
                std::lock_guard lock(sub->stateMutex);
                if (--sub->activeCalls == 0) sub->idle.notify_all();
                throw;
            }
            {
                std::lock_guard lock(sub->stateMutex);
                if (--sub->activeCalls == 0) sub->idle.notify_all();
            }
        }
    }

    /// 获取指定事件类型的订阅者数量
    template <typename TEvent>
    size_t subscriberCount() const {
        std::lock_guard lock(m_mutex);
        auto typeId = std::type_index(typeid(TEvent));
        auto it = m_subscribers.find(typeId);
        return (it != m_subscribers.end()) ? it->second.size() : 0;
    }

    /// 移除所有订阅
    void clearAll();

private:
    EventBus() = default;
    EventBus(const EventBus&) = delete;
    EventBus& operator=(const EventBus&) = delete;

    struct Subscription {
        SubscriptionId id = 0;
        std::function<void(const std::any&)> handler;
        std::mutex stateMutex;
        std::condition_variable idle;
        size_t activeCalls = 0;
        bool accepting = true;
    };

    std::shared_ptr<Subscription> detach(SubscriptionId id);
    static void retire(const std::shared_ptr<Subscription>& subscription, bool wait);

    mutable std::mutex m_mutex;
    std::unordered_map<std::type_index, std::vector<std::shared_ptr<Subscription>>> m_subscribers;
    std::unordered_map<SubscriptionId, std::type_index> m_idToType;
    std::atomic<SubscriptionId> m_nextId{1};
};

// ─────────────────────────────────────────────────────────────────────────────
// RAII 订阅守卫 — 作用域退出时自动取消订阅
// ─────────────────────────────────────────────────────────────────────────────

class ScopedSubscription {
public:
    ScopedSubscription() = default;
    explicit ScopedSubscription(SubscriptionId id) : m_id(id) {}

    ~ScopedSubscription() {
        if (m_id != 0) {
            EventBus::instance().unsubscribe(m_id);
        }
    }

    // Move only
    ScopedSubscription(ScopedSubscription&& other) noexcept : m_id(other.m_id) { other.m_id = 0; }
    ScopedSubscription& operator=(ScopedSubscription&& other) noexcept {
        if (this != &other) {
            if (m_id != 0) EventBus::instance().unsubscribe(m_id);
            m_id = other.m_id;
            other.m_id = 0;
        }
        return *this;
    }

    ScopedSubscription(const ScopedSubscription&) = delete;
    ScopedSubscription& operator=(const ScopedSubscription&) = delete;

    SubscriptionId id() const { return m_id; }

private:
    SubscriptionId m_id = 0;
};

}  // namespace tools3000::core

#endif  // TOOLS3000_CORE_EVENTS_EVENTBUS_H
