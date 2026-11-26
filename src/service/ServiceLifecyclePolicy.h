#pragma once
// ─────────────────────────────────────────────────────────────────────────────
// ServiceLifecyclePolicy — 搜索服务 SCM 生命周期裁决纯策略（零 Win32 运行时副作用）
//
// 核心契约：
//   1. 搜索服务保持 DEMAND_START（按需启动），绝不开机自启；
//   2. 在当前 Windows 登录会话内跨 Tools3000 退出、锁屏、睡眠、远程断开保持常驻；
//   3. 当 Windows 关机（含 Win10/11 默认开启的「快速启动 / 混合关机」，内核会先注销
//      全部交互式用户会话再将 Session 0 休眠进 hiberfil.sys）或所有登录用户均注销时，
//      先落盘最新 USN 索引快照并优雅退出服务，确保下一次按电源键开机保持真正的 0 进程、0 MB。
// ─────────────────────────────────────────────────────────────────────────────

#ifndef TOOLS3000_SERVICE_SERVICELIFECYCLEPOLICY_H
#define TOOLS3000_SERVICE_SERVICELIFECYCLEPOLICY_H

#include <windows.h>
#include <wtsapi32.h>

#include <cstdint>
#include <span>

namespace tools3000::service::lifecycle {

inline constexpr uint32_t kInvalidSessionId = 0xFFFFFFFFu;
inline constexpr DWORD kGracefulStopWaitHintMs = 8000u;

struct SessionDescriptor {
    uint32_t sessionId = 0;
    WTS_CONNECTSTATE_CLASS state = WTSDown;
};

/// SCM 注册可接受控制码：
/// 注意依据 MSDN 规范，绝不同时声明 SERVICE_ACCEPT_SHUTDOWN 与 SERVICE_ACCEPT_PRESHUTDOWN。
[[nodiscard]] constexpr DWORD acceptedServiceControls() noexcept {
    return SERVICE_ACCEPT_STOP | SERVICE_ACCEPT_SHUTDOWN | SERVICE_ACCEPT_SESSIONCHANGE;
}

/// 判断 SCM 控制码是否要求立即触发优雅落盘停服
[[nodiscard]] constexpr bool shouldStopOnServiceControl(DWORD ctrlCode) noexcept {
    return ctrlCode == SERVICE_CONTROL_STOP ||
           ctrlCode == SERVICE_CONTROL_SHUTDOWN ||
           ctrlCode == SERVICE_CONTROL_PRESHUTDOWN;
}

/// 判断一条 WTS 会话记录是否属于「仍在登录态的交互式用户会话」
/// - 自动排除 Session 0（系统服务会话）
/// - 自动排除正在注销中的 excludedSessionId（避免 WTS_SESSION_LOGOFF 瞬间列表尚未摘除导致误判）
/// - 严格排除 WTSListen（如 RDP-Tcp 监听会话）、WTSIdle、WTSDown、WTSInit、WTSReset
[[nodiscard]] constexpr bool isInteractiveLoggedInSession(
    const SessionDescriptor& session,
    uint32_t excludedSessionId = kInvalidSessionId) noexcept {
    if (session.sessionId == 0 || session.sessionId == excludedSessionId) {
        return false;
    }
    switch (session.state) {
        case WTSActive:
        case WTSConnected:
        case WTSConnectQuery:
        case WTSShadow:
        case WTSDisconnected:
            return true;
        case WTSListen:
        case WTSReset:
        case WTSDown:
        case WTSInit:
        case WTSIdle:
        default:
            return false;
    }
}

/// 统计排除刚注销会话后，机器上剩余的已登录交互式用户会话数
[[nodiscard]] inline uint32_t countRemainingInteractiveSessions(
    std::span<const SessionDescriptor> sessions,
    uint32_t excludedSessionId = kInvalidSessionId) noexcept {
    uint32_t count = 0;
    for (const auto& s : sessions) {
        if (isInteractiveLoggedInSession(s, excludedSessionId)) {
            ++count;
        }
    }
    return count;
}

/// 裁决收到 SERVICE_CONTROL_SESSIONCHANGE 时是否应当落盘停服：
/// - 仅在 WTS_SESSION_LOGOFF 且已无任何剩余登录用户会话时停服（覆盖单用户注销与快速启动混合关机）；
/// - 锁屏、解锁、远程断连、切屏、多用户仍有他人在登录态时一律保持运行。
[[nodiscard]] constexpr bool shouldStopOnSessionChange(
    DWORD eventType,
    uint32_t remainingInteractiveSessions) noexcept {
    return eventType == WTS_SESSION_LOGOFF && remainingInteractiveSessions == 0;
}

}  // namespace tools3000::service::lifecycle

#endif  // TOOLS3000_SERVICE_SERVICELIFECYCLEPOLICY_H
