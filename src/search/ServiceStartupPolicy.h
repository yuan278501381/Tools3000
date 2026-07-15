#pragma once

#include <windows.h>

// 搜索服务启动判定保持纯函数：SCM 的状态与客户端是否已连接到本用户端点
// 被分离，单元测试不依赖真实服务、磁盘或管理员权限。

namespace tools3000::search {

enum class ScmServiceState {
    Missing,
    Stopped,
    StartPending,
    Running,
    StopPending,
    Failed,
};

enum class StartupAction {
    UseEndpoint,
    StartScmService,
    WaitForScmEndpoint,
    AllowPortableFallback,
    ReportUnavailable,
};

inline StartupAction decideStartupAction(
    bool endpointReady, ScmServiceState state, bool scmStartExplicitlyFailed) noexcept {
    if (endpointReady) return StartupAction::UseEndpoint;
    switch (state) {
        case ScmServiceState::Missing:
            return StartupAction::AllowPortableFallback;
        case ScmServiceState::Stopped:
            return scmStartExplicitlyFailed ? StartupAction::AllowPortableFallback
                                             : StartupAction::StartScmService;
        case ScmServiceState::StartPending:
        case ScmServiceState::Running:
            // A running SCM instance without this endpoint is not evidence that
            // starting a portable instance is safe. Keep the singleton intact.
            return StartupAction::WaitForScmEndpoint;
        case ScmServiceState::StopPending:
        case ScmServiceState::Failed:
            return StartupAction::ReportUnavailable;
    }
    return StartupAction::ReportUnavailable;
}

/// OpenService(SERVICE_START|QUERY) 常因普通用户没有启动权限而 ACCESS_DENIED，
/// 并不表示服务不存在。此时应降级为只查询；查不到或服务已停止则走便携进程。
inline bool scmOpenShouldRetryQueryOnly(DWORD openError) noexcept {
    return openError == ERROR_ACCESS_DENIED;
}

/// Once a SCM service is RUNNING it must publish the authenticated per-user
/// endpoint promptly. Reaching the bounded wait deadline without that endpoint
/// means its launch credentials belong to another interactive user/session (or
/// are stale), not that portable fallback is safe.
inline bool isScmEndpointIdentityConflict(
    ScmServiceState state, bool endpointReady, bool deadlineExpired) noexcept {
    return deadlineExpired && !endpointReady && state == ScmServiceState::Running;
}

}  // namespace tools3000::search
