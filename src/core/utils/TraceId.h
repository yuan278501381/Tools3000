#pragma once
// ─────────────────────────────────────────────────────────────────────────────
// TraceId — 分布式追踪 ID 生成器
// 格式: YYYYMMDD-HHmmss-XXXX (日期-时间-随机4位)
// 用途: 每个用户操作/请求分配唯一 TraceId，贯穿整条日志链
// ─────────────────────────────────────────────────────────────────────────────

#ifndef TOOLS3000_CORE_UTILS_TRACEID_H
#define TOOLS3000_CORE_UTILS_TRACEID_H

#include "core/utils/Export.h"

#include <string>
#include <chrono>
#include <random>
#include <format>
#include <atomic>

namespace tools3000::core {

class TOOLS3000CORE_API TraceId {
public:
    /// 生成一个新的 TraceId
    /// @return 格式: "20260619-180215-A3F7"
    static std::string generate();

    /// 获取当前线程的活跃 TraceId
    static const std::string& current();

    /// 设置当前线程的活跃 TraceId（用于跨线程传递）
    static void setCurrent(const std::string& traceId);

    /// 开启一个全新的 TraceId 并返回它（标记一次新的用户操作的开始）
    static std::string begin();

    /// RAII 作用域守卫 —— 在作用域内自动设置/恢复 TraceId
    class Scope {
    public:
        explicit Scope(const std::string& traceId = TraceId::generate()) : m_previous(TraceId::current()) {
            TraceId::setCurrent(traceId);
        }
        ~Scope() { TraceId::setCurrent(m_previous); }

        Scope(const Scope&) = delete;
        Scope& operator=(const Scope&) = delete;

        [[nodiscard]] const std::string& id() const { return TraceId::current(); }

    private:
        std::string m_previous;
    };

private:
    /// 单一的线程局部存储 —— 下沉至 Tools3000Core.dll 编译单元，彻底杜绝跨 DLL thread_local 脑裂
    static std::string& storage();
};

}  // namespace tools3000::core

#endif  // TOOLS3000_CORE_UTILS_TRACEID_H
