// ─────────────────────────────────────────────────────────────────────────────
// SessionIsolation.cpp — Windows 多会话并发隔离与 WebView2 槽位租约引擎实现
//
// Copyright (c) 2026 Yy1 (yuan278501381) & Tools3000 contributors. GPL-3.0 License.
// ─────────────────────────────────────────────────────────────────────────────

#include "core/utils/SessionIsolation.h"

#include <chrono>
#include <cwctype>
#include <system_error>

namespace tools3000::core {
namespace {

constexpr const wchar_t* kPrimaryWebViewDirName = L"webview2_data";
constexpr const wchar_t* kSecondaryWebViewPrefix = L"webview2_data_slot_";
constexpr const wchar_t* kSlotLockFileName = L".tools3000_slot.lock";
constexpr const wchar_t* kSessionSingleInstanceMutex = L"Local\\Tools3000_SingleInstance_Mutex";
constexpr const wchar_t* kLifecycleTestMutexPrefix = L"Local\\Tools3000_LifecycleTest_";

HANDLE tryAcquireDirectorySlotLock(const std::filesystem::path& slotDir) noexcept {
    std::error_code ec;
    std::filesystem::create_directories(slotDir, ec);
    if (ec) {
        return INVALID_HANDLE_VALUE;
    }

    const std::filesystem::path lockPath = slotDir / kSlotLockFileName;
    // 严格独占不共享 (dwShareMode = 0) + FILE_FLAG_DELETE_ON_CLOSE：
    // 进程正常退出或异常被杀时，Windows 内核瞬间释放锁并自动删除锁文件，彻底杜绝僵尸锁。
    HANDLE handle = CreateFileW(
        lockPath.c_str(),
        GENERIC_WRITE,
        0,
        nullptr,
        CREATE_ALWAYS,
        FILE_ATTRIBUTE_HIDDEN | FILE_FLAG_DELETE_ON_CLOSE,
        nullptr
    );
    return handle;
}

bool isSecondarySlotDirectoryName(const std::wstring& name) {
    const std::wstring prefix(kSecondaryWebViewPrefix);
    if (name.size() <= prefix.size() || name.compare(0, prefix.size(), prefix) != 0) {
        return false;
    }
    for (size_t i = prefix.size(); i < name.size(); ++i) {
        if (!std::iswdigit(name[i])) {
            return false;
        }
    }
    return true;
}

bool isFileOlderThanDays(const std::filesystem::path& target, int retentionDays) {
    if (retentionDays <= 0) {
        return true;
    }
    std::error_code ec;
    const auto writeTime = std::filesystem::last_write_time(target, ec);
    if (ec) {
        return false;
    }
    const auto now = std::filesystem::file_time_type::clock::now();
    const auto threshold = std::chrono::hours(static_cast<int64_t>(retentionDays) * 24LL);
    return (now > writeTime) && ((now - writeTime) >= threshold);
}

}  // namespace

// ── WebViewProfileSlotLease ──────────────────────────────────────────────────

WebViewProfileSlotLease::WebViewProfileSlotLease() noexcept = default;

WebViewProfileSlotLease::WebViewProfileSlotLease(
    HANDLE lockHandle,
    std::filesystem::path dataDirectory,
    int slotIndex) noexcept
    : m_lockHandle(lockHandle)
    , m_dataDirectory(std::move(dataDirectory))
    , m_slotIndex(slotIndex) {}

WebViewProfileSlotLease::~WebViewProfileSlotLease() {
    release();
}

WebViewProfileSlotLease::WebViewProfileSlotLease(WebViewProfileSlotLease&& other) noexcept
    : m_lockHandle(other.m_lockHandle)
    , m_dataDirectory(std::move(other.m_dataDirectory))
    , m_slotIndex(other.m_slotIndex) {
    other.m_lockHandle = INVALID_HANDLE_VALUE;
    other.m_slotIndex = -1;
}

WebViewProfileSlotLease& WebViewProfileSlotLease::operator=(WebViewProfileSlotLease&& other) noexcept {
    if (this != &other) {
        release();
        m_lockHandle = other.m_lockHandle;
        m_dataDirectory = std::move(other.m_dataDirectory);
        m_slotIndex = other.m_slotIndex;
        other.m_lockHandle = INVALID_HANDLE_VALUE;
        other.m_slotIndex = -1;
    }
    return *this;
}

void WebViewProfileSlotLease::release() noexcept {
    if (m_lockHandle != INVALID_HANDLE_VALUE && m_lockHandle != nullptr) {
        CloseHandle(m_lockHandle);
        m_lockHandle = INVALID_HANDLE_VALUE;
    }
    m_slotIndex = -1;
}

bool WebViewProfileSlotLease::isHeld() const noexcept {
    return m_lockHandle != INVALID_HANDLE_VALUE && m_lockHandle != nullptr;
}

int WebViewProfileSlotLease::slotIndex() const noexcept {
    return m_slotIndex;
}

bool WebViewProfileSlotLease::isPrimarySlot() const noexcept {
    return m_slotIndex == 0;
}

const std::filesystem::path& WebViewProfileSlotLease::dataDirectory() const noexcept {
    return m_dataDirectory;
}

// ── SessionIsolation ─────────────────────────────────────────────────────────

DWORD SessionIsolation::currentSessionId() noexcept {
    DWORD sessionId = 0;
    if (ProcessIdToSessionId(GetCurrentProcessId(), &sessionId)) {
        return sessionId;
    }
    return 0;
}

std::wstring SessionIsolation::buildSingleInstanceMutexName(
    bool lifecycleTestInstance,
    DWORD processId) {
    if (lifecycleTestInstance) {
        return std::wstring(kLifecycleTestMutexPrefix) + std::to_wstring(processId);
    }
    return std::wstring(kSessionSingleInstanceMutex);
}

WebViewProfileSlotLease SessionIsolation::acquireWebViewProfileSlot(
    const std::filesystem::path& appDataRoot,
    int maxSecondarySlots) {
    const int safeMaxSecondary = (maxSecondarySlots < 1) ? 1 : maxSecondarySlots;

    // 1. 优先尝试抢占主槽位 Slot 0 (webview2_data)
    const std::filesystem::path primaryDir = appDataRoot / kPrimaryWebViewDirName;
    if (HANDLE hPrimary = tryAcquireDirectorySlotLock(primaryDir); hPrimary != INVALID_HANDLE_VALUE) {
        // 仅在拥有主槽位时静默清扫超时未用的过期副槽位，保持磁盘长期整洁
        (void)pruneStaleSecondaryWebViewSlots(appDataRoot, kStaleSecondarySlotRetentionDays);
        return WebViewProfileSlotLease(hPrimary, primaryDir, 0);
    }

    // 2. 主槽位正被同账号其他并发会话占用 -> 平滑探测并复用空闲副槽位 Slot 1..N
    for (int slot = 1; slot <= safeMaxSecondary; ++slot) {
        const std::filesystem::path secondaryDir =
            appDataRoot / (std::wstring(kSecondaryWebViewPrefix) + std::to_wstring(slot));
        if (HANDLE hSlot = tryAcquireDirectorySlotLock(secondaryDir); hSlot != INVALID_HANDLE_VALUE) {
            return WebViewProfileSlotLease(hSlot, secondaryDir, slot);
        }
    }

    // 3. 极端并发兜底（超过 16 个同账号并发会话）：分配独立 PID 目录保障永不失败
    const std::filesystem::path fallbackDir =
        appDataRoot / (L"webview2_data_pid_" + std::to_wstring(GetCurrentProcessId()));
    HANDLE hFallback = tryAcquireDirectorySlotLock(fallbackDir);
    return WebViewProfileSlotLease(hFallback, fallbackDir, safeMaxSecondary + 1);
}

size_t SessionIsolation::pruneStaleSecondaryWebViewSlots(
    const std::filesystem::path& appDataRoot,
    int retentionDays) {
    size_t prunedCount = 0;
    std::error_code ec;
    if (!std::filesystem::exists(appDataRoot, ec) || !std::filesystem::is_directory(appDataRoot, ec)) {
        return 0;
    }

    for (const auto& entry : std::filesystem::directory_iterator(appDataRoot, ec)) {
        if (ec) break;
        std::error_code itemEc;
        if (!entry.is_directory(itemEc) || itemEc) continue;

        const std::wstring dirName = entry.path().filename().wstring();
        if (!isSecondarySlotDirectoryName(dirName)) continue;

        if (!isFileOlderThanDays(entry.path(), retentionDays)) continue;

        // 必须先成功抢占独占槽位锁再清理，100% 杜绝误删正在被活跃会话使用的副槽位
        HANDLE probeLock = tryAcquireDirectorySlotLock(entry.path());
        if (probeLock == INVALID_HANDLE_VALUE) {
            continue;
        }
        CloseHandle(probeLock);

        std::error_code rmEc;
        std::filesystem::remove_all(entry.path(), rmEc);
        if (!rmEc) {
            ++prunedCount;
        }
    }
    return prunedCount;
}

}  // namespace tools3000::core
