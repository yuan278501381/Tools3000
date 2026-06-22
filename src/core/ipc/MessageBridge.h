#pragma once
// ─────────────────────────────────────────────────────────────────────────────
// MessageBridge — WebView2 ↔ C++ IPC 桥接层
//
// 职责:
//   1. 定义 JS ↔ C++ 通信协议（JSON-RPC 风格）
//   2. 路由前端请求到正确的 C++ 处理器
//   3. 推送 C++ 事件到前端
// ─────────────────────────────────────────────────────────────────────────────

#ifndef TOOLS3000_CORE_IPC_MESSAGEBRIDGE_H
#define TOOLS3000_CORE_IPC_MESSAGEBRIDGE_H

#include "core/utils/Export.h"

#include <string>
#include <condition_variable>
#include <functional>
#include <memory>
#include <mutex>
#include <shared_mutex>
#include <unordered_map>
#include <unordered_set>
#include <nlohmann/json.hpp>

namespace tools3000::core {

using json = nlohmann::json;

/// 消息处理器类型: 接收请求参数，返回响应
using MessageHandler = std::function<json(const json& params)>;

/// 事件推送器类型: 由 WebView2 层实现，用于向 JS 发送消息
using EventPusher = std::function<void(const std::string& eventName, const json& data)>;

/// 异步响应回调。可能在任意工作线程上被调用，调用方需自行切回所需线程。
using AsyncResponder = std::function<void(std::string responseJson)>;

class TOOLS3000CORE_API MessageBridge {
public:
    static MessageBridge& instance();

    // ── 注册处理器 (C++ 侧) ──────────────────────────────────────────────

    /// 注册消息处理器
    /// @param method 方法名 (如 "gesture.getProfiles", "config.get")
    void registerHandler(const std::string& method, MessageHandler handler);

    /// 注册宿主（Tools3000.exe / Tools3000Core.dll）内置消息处理器。
    /// 按插件前缀注销时（unregisterHandlersByPrefix）将保留宿主处理器，仅在进程退出 clearHandlers() 时释放。
    void registerHostHandler(const std::string& method, MessageHandler handler);

    /// 注销一个处理器，并等待已经进入该处理器的调用结束。
    void unregisterHandler(const std::string& method);

    /// 按命名空间前缀注销插件处理器（如 "capture."），并等待在途调用结束。
    size_t unregisterHandlersByPrefix(const std::string& prefix);

    /// 是否存在因在途调用超时而隔离保留的处理器或异步工作线程（若存在则不可 FreeLibrary 卸载插件 DLL）
    bool hasQuarantinedHandlers() const noexcept;

    // ── 消息处理 ─────────────────────────────────────────────────────────

    /// 处理来自前端的消息
    /// @param messageJson JSON-RPC 风格消息: {"id": 1, "method": "xxx", "params": {...}}
    /// @return 响应 JSON: {"id": 1, "result": {...}} 或 {"id": 1, "error": {...}}
    std::string handleMessage(const std::string& messageJson);

    /// 将方法标记为异步执行。被标记的方法会在后台线程池中运行，调用
    /// handleMessageAsync 的线程（通常是 WebView2 的 UI 线程）不会被阻塞。
    /// 适用于跨进程、跨磁盘等耗时不可控的处理器。
    void markMethodAsync(const std::string& method);

    /// 处理来自前端的消息，结果通过 responder 回调返回。
    /// 方法已标记为异步时投递到线程池并立即返回，responder 在工作线程被调用；
    /// 否则同步执行，responder 在本函数返回前被调用。
    void handleMessageAsync(const std::string& messageJson, AsyncResponder responder);

    /// 判断是否已注册特定方法
    bool hasHandler(const std::string& method) const;

    /// 进程内直接调用已注册的处理器
    json invokeHandler(const std::string& method, const json& params = json::object());

    /// 设置事件推送器（由 WebView2 初始化时调用，兼容模式）
    void setEventPusher(EventPusher pusher);

    /// 注册具名事件推送器（支持多个 WebView 窗口同时接收事件）
    void registerEventPusher(const std::string& name, EventPusher pusher);

    /// 注销具名事件推送器
    void unregisterEventPusher(const std::string& name);

    /// 清空全部处理器和事件推送器。插件 DLL 卸载前必须调用，避免遗留
    /// std::function 的析构代码指向已经卸载的模块。
    void clearHandlers();

    /// 向前端推送事件
    void pushEvent(const std::string& eventName, const json& data = {});

    // ── 预置处理器注册 ──────────────────────────────────────────────────

    /// 注册所有内置处理器（配置、手势、截图等）
    void registerBuiltinHandlers();

private:
    MessageBridge() = default;

    struct HandlerSlot {
        MessageHandler handler;
        std::mutex mutex;
        std::condition_variable idle;
        size_t activeCalls = 0;
        bool accepting = true;
        bool isHostOwned = false;
    };

    struct WorkerPool;

    void registerHandlerImpl(const std::string& method, MessageHandler handler, bool isHostOwned);
    static void retireSlots(std::vector<std::shared_ptr<HandlerSlot>> slots,
                            bool boundedForShutdown = false);

    /// 惰性创建线程池。仅在确实注册了异步方法时才付出线程开销。
    WorkerPool& ensureWorkerPool();
    void shutdownWorkerPool();

    std::unordered_map<std::string, std::shared_ptr<HandlerSlot>> m_handlers;
    std::unordered_set<std::string> m_asyncMethods;
    WorkerPool* m_workerPool = nullptr;
    std::unordered_map<std::string, EventPusher> m_eventPushers;
    mutable std::shared_mutex m_mutex;
};

}  // namespace tools3000::core

#endif  // TOOLS3000_CORE_IPC_MESSAGEBRIDGE_H
