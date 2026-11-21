// ─────────────────────────────────────────────────────────────────────────────
// SessionIsolation.h — Windows 多会话并发隔离与 WebView2 槽位租约引擎
//
// Copyright (c) 2026 Yy1 (yuan278501381) & Tools3000 contributors. GPL-3.0 License.
// ─────────────────────────────────────────────────────────────────────────────
#pragma once

#include "core/utils/Export.h"

#include <windows.h>
#include <cstdint>
#include <filesystem>
#include <string>

namespace tools3000::core {

/// RAII 独占槽位租约：持有槽位锁文件内核句柄，保证跨会话并发运行零 Profile 冲突；
/// 无论正常退出还是异常崩溃强杀，操作系统内核都会立即释放句柄并清除锁文件。
class TOOLS3000CORE_API WebViewProfileSlotLease final {
public:
    WebViewProfileSlotLease() noexcept;
    ~WebViewProfileSlotLease();

    WebViewProfileSlotLease(const WebViewProfileSlotLease&) = delete;
    WebViewProfileSlotLease& operator=(const WebViewProfileSlotLease&) = delete;

    WebViewProfileSlotLease(WebViewProfileSlotLease&& other) noexcept;
    WebViewProfileSlotLease& operator=(WebViewProfileSlotLease&& other) noexcept;

    void release() noexcept;

    [[nodiscard]] bool isHeld() const noexcept;
    [[nodiscard]] int slotIndex() const noexcept;
    [[nodiscard]] bool isPrimarySlot() const noexcept;
    [[nodiscard]] const std::filesystem::path& dataDirectory() const noexcept;

private:
    friend class SessionIsolation;
    WebViewProfileSlotLease(HANDLE lockHandle,
                            std::filesystem::path dataDirectory,
                            int slotIndex) noexcept;

    HANDLE m_lockHandle = INVALID_HANDLE_VALUE;
    std::filesystem::path m_dataDirectory;
    int m_slotIndex = -1;
};

/// Windows Server / 多管理员并发会话隔离单一事实源
class TOOLS3000CORE_API SessionIsolation final {
public:
    static constexpr int kMaxSecondaryWebViewSlots = 15;
    static constexpr int kStaleSecondarySlotRetentionDays = 7;

    /// 获取当前进程所属的 Windows Terminal Services Session ID
    static DWORD currentSessionId() noexcept;

    /// 构建会话级单实例互斥体名称：
    /// - 正常启动：Local\Tools3000_SingleInstance_Mutex（同会话严格单实例，跨会话独立并行）
    /// - 生命周期门禁：Local\Tools3000_LifecycleTest_<pid>
    static std::wstring buildSingleInstanceMutexName(bool lifecycleTestInstance,
                                                     DWORD processId) ;

    /// 抢占一个进程独占的 WebView2 UserDataFolder 槽位：
    /// 1. 优先抢占主槽位 Slot 0 (<appDataRoot>/webview2_data)，保证单机与首会话 0 迁移、0 缓存丢失；
    /// 2. 当同一账号在 Windows Server 多会话并发运行时，后续会话自动平滑降级抢占 Slot 1..15
    ///    (<appDataRoot>/webview2_data_slot_<N>)，彻底根除 Chromium SingletonLock 冲突；
    /// 3. 槽位释放后自动复用于下次并发会话，杜绝目录无限膨胀。
    static WebViewProfileSlotLease acquireWebViewProfileSlot(
        const std::filesystem::path& appDataRoot,
        int maxSecondarySlots = kMaxSecondaryWebViewSlots);

    /// 清理不再被任何进程锁定且空闲超过 retentionDays 天的过期副槽位目录，返回已清理数量
    static size_t pruneStaleSecondaryWebViewSlots(
        const std::filesystem::path& appDataRoot,
        int retentionDays = kStaleSecondarySlotRetentionDays);
};

}  // namespace tools3000::core
