/**
 * Tools3000 - High Performance Windows Productivity Suite
 *
 * Copyright (c) 2026 Yy1 (GitHub yuan278501381) <https://github.com/yuan278501381> & Tools3000 contributors
 *
 * Licensed under the GPL-3.0 License.
 */

#pragma once

#ifndef TOOLS3000_CORE_IPC_SEARCHPIPECLIENT_H
#define TOOLS3000_CORE_IPC_SEARCHPIPECLIENT_H

#include "core/utils/Export.h"

#include <windows.h>

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace tools3000::core::ipc {

/// 搜索服务返回的单条文件/目录结果
struct SearchFileItem {
    std::string name;
    std::string path;
    bool isDirectory{false};
    uint64_t size{0};
    uint64_t lastWriteTime{0};
    double matchRank{0.0};
    double frecencyScore{0.0};
};

/// 管道查询状态枚举
enum class SearchPipeStatus : uint8_t {
    Ready = 0,
    Starting = 1,
    Busy = 2,
    Timeout = 3,
    Cancelled = 4,
    ServiceNotRunning = 5,
    Unavailable = 6,
    Error = 7,
};

/// 管道文件查询响应
struct SearchQueryResponse {
    SearchPipeStatus status{SearchPipeStatus::Unavailable};
    std::vector<SearchFileItem> items;
    uint64_t queryId{0};
    int64_t elapsedMs{0};
    bool initializing{false};
    bool cancelled{false};
    DWORD win32Error{0};
    std::string errorMessage;
};

/// 文件对话框上下文优先目录加权项
struct DialogPriorityEntry {
    std::string path;
    double score{4000.0};
};

/// 跨插件共享的搜索服务命名管道客户端（纯 Unicode *W、线程安全、支持 CancelIoEx 与严格 DEMAND_START）
///
/// 架构契约：
/// 1. DLL 导出类严格遵循非内联规范：所有成员方法及静态单例实现均位于 SearchPipeClient.cpp。
/// 2. 严格守护 DEMAND_START 不变量：autoStart == false 或查询为空时绝不拉起 Tools3000_Service.exe。
/// 3. 与 Plugin_Search 共享跨进程互斥量 Local\Tools3000SearchLaunch-<SID>，杜绝重复拉起索引服务。
class TOOLS3000CORE_API SearchPipeClient {
public:
    static SearchPipeClient& instance();

    /// 纯只读探测当前是否有可用的搜索服务管道（零副作用，绝不启动服务）
    bool isServiceRunningNow(DWORD waitMs = 1) const;

    /// 显式按需异步唤醒搜索服务（仅允许在用户主动发起搜索输入时调用）
    void warmupAsync(std::function<void(bool ready)> onReady = {});

    /// 检查当前是否已有后台按需启动任务在途
    bool isWarmupInFlight() const noexcept;

    /// 查询全盘文件索引（name 极速模式，5-15ms）
    /// @param rawQuery         用户输入的原始查询字符串
    /// @param queryId          单调递增查询序号，用于乱序丢弃与服务端取消联动
    /// @param limit            最大返回条数
    /// @param onlyFolders      为 true 时自动约束为仅返回目录（自动附加 folder: 过滤器）
    /// @param dialogPriorities 对话框当前目录/收藏工作区等高优路径权重列表
    /// @param autoStart        仅当用户显式输入查询时才允许置 true；为 false 时服务未运行绝不拉起
    /// @param timeoutMs        本次查询最大超时时间（毫秒）
    /// @param externalCancel   可选的外部取消标志；置 true 后通过 CancelIoEx 立即中止挂起 I/O
    SearchQueryResponse queryFiles(
        const std::string& rawQuery,
        uint64_t queryId,
        size_t limit = 30,
        bool onlyFolders = false,
        const std::vector<DialogPriorityEntry>& dialogPriorities = {},
        bool autoStart = false,
        DWORD timeoutMs = 1500,
        const std::atomic<bool>* externalCancel = nullptr);

    /// 立即取消所有在途的管道 I/O（调用 CancelIoEx）
    void cancelActiveQueries();

    /// 停止并回收后台异步唤醒线程（仅在进程退出或隔离测试清理时调用）
    void shutdown();

    /// 构造实际下发给搜索服务的查询表达式（按需无重复拼接 folder: 过滤前缀）
    static std::string buildEffectiveQuery(const std::string& rawQuery, bool onlyFolders);

    /// DEMAND_START 启动守卫纯判定函数（确保空白输入或未显式输入绝不拉起搜索服务）
    static bool shouldAllowAutoStart(
        const std::string& rawQuery,
        bool explicitUserTyped,
        bool autoStartRequested) noexcept;

private:
    SearchPipeClient();
    ~SearchPipeClient();
    SearchPipeClient(const SearchPipeClient&) = delete;
    SearchPipeClient& operator=(const SearchPipeClient&) = delete;

    struct Impl;
    Impl* m_impl;
};

}  // namespace tools3000::core::ipc

#endif  // TOOLS3000_CORE_IPC_SEARCHPIPECLIENT_H
