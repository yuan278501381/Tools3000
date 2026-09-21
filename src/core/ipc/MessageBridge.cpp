// ─────────────────────────────────────────────────────────────────────────────
// MessageBridge.cpp — WebView2 ↔ C++ IPC 桥接层实现
// ─────────────────────────────────────────────────────────────────────────────

#include "core/ipc/MessageBridge.h"
#include "core/ipc/SystemInteractionHandlers.h"
#include "core/ipc/AutoStartPolicy.h"
#include "core/logger/Logger.h"
#include "core/plugin/PluginManager.h"
#include "core/utils/TraceId.h"
#include "core/config/ConfigManager.h"
#include "core/hotkey/HotkeyManager.h"
#include "core/utils/WinUtils.h"
#include "core/stats/StatsManager.h"
#include "core/stats/PerformanceMonitor.h"
#include "core/update/UpdateChecker.h"
#include "core/events/EventBus.h"
#include "core/remote/RemoteMasterEngine.h"
#include "Tools3000Version.h"
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <condition_variable>
#include <deque>
#include <filesystem>
#include <memory>
#include <mutex>
#include <optional>
#include <string_view>
#include <thread>
#include <unordered_set>
#include <utility>
#include <vector>
#include <shobjidl.h>
#include <shellapi.h>

namespace {

using json = nlohmann::json;
constexpr size_t MaxBridgeMessageBytes = 1024 * 1024;

std::string bridgeErrorResponse(int id, int code, std::string_view message) {
    return json{{"id", id}, {"error", {{"code", code}, {"message", std::string(message)}}}}.dump();
}

std::optional<std::filesystem::path> choosePath(bool save, bool folder = false) {
    IFileDialog* dialog = nullptr;
    const CLSID clsid = save ? CLSID_FileSaveDialog : CLSID_FileOpenDialog;
    HRESULT hr = CoCreateInstance(clsid, nullptr, CLSCTX_INPROC_SERVER,
                                  IID_PPV_ARGS(&dialog));
    if (FAILED(hr) || !dialog) return std::nullopt;

    if (folder) {
        DWORD options = 0;
        dialog->GetOptions(&options);
        dialog->SetOptions(options | FOS_PICKFOLDERS | FOS_FORCEFILESYSTEM);
        dialog->SetTitle(L"选择保存目录");
    } else {
        static const COMDLG_FILTERSPEC filters[] = {
            {L"Tools3000 配置 (*.json)", L"*.json"},
            {L"所有文件 (*.*)", L"*.*"},
        };
        dialog->SetFileTypes(static_cast<UINT>(std::size(filters)), filters);
        dialog->SetDefaultExtension(L"json");
        dialog->SetFileName(save ? L"Tools3000-config.json" : L"");
        dialog->SetTitle(save ? L"导出 Tools3000 配置" : L"导入 Tools3000 配置");
    }

    std::optional<std::filesystem::path> result;
    hr = dialog->Show(nullptr);
    if (SUCCEEDED(hr)) {
        IShellItem* item = nullptr;
        if (SUCCEEDED(dialog->GetResult(&item)) && item) {
            PWSTR rawPath = nullptr;
            if (SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &rawPath)) && rawPath) {
                result = std::filesystem::path(rawPath);
                CoTaskMemFree(rawPath);
            }
            item->Release();
        }
    }
    dialog->Release();
    return result;
}

bool isPathWithin(const std::filesystem::path& candidate,
                  const std::filesystem::path& directory) {
    auto candidateIt = candidate.begin();
    for (auto directoryIt = directory.begin(); directoryIt != directory.end();
         ++directoryIt, ++candidateIt) {
        if (candidateIt == candidate.end() ||
            _wcsicmp(directoryIt->c_str(), candidateIt->c_str()) != 0) {
            return false;
        }
    }
    return true;
}

// A path selected by the native picker carries an explicit user gesture and is
// allowed outside AppData. A path supplied directly by WebView IPC does not;
// constrain that untrusted route to the application's config directory.
bool isApprovedDirectConfigPath(const std::filesystem::path& input, bool importing) {
    if (input.empty() || !input.is_absolute() ||
        tools3000::core::WinUtils::toLower(input.extension().wstring()) != L".json" ||
        std::any_of(input.begin(), input.end(), [](const auto& component) {
            return component == L"..";
        })) {
        return false;
    }

    std::error_code ec;
    const auto approvedDirectory = std::filesystem::canonical(
        tools3000::core::WinUtils::getConfigDirectory(), ec);
    if (ec) return false;

    std::filesystem::path resolved;
    if (importing) {
        resolved = std::filesystem::canonical(input, ec);
    } else {
        const auto parent = std::filesystem::canonical(input.parent_path(), ec);
        if (!ec) resolved = (parent / input.filename()).lexically_normal();
    }
    return !ec && isPathWithin(resolved, approvedDirectory);
}

bool setAutoStart(bool enabled);

namespace {

constexpr wchar_t AUTOSTART_TASK_NAME[] = L"Tools3000_Autostart";

bool executeSilentCommand(const std::wstring& cmd, std::string* capturedOutput = nullptr) {
    constexpr std::size_t MaxCapturedOutputBytes = 1024u * 1024u;
    STARTUPINFOEXW startupInfo{};
    STARTUPINFOW& si = startupInfo.StartupInfo;
    si.cb = sizeof(STARTUPINFOW);
    si.dwFlags = STARTF_USESHOWWINDOW;
    si.wShowWindow = SW_HIDE;

    HANDLE readPipe = nullptr;
    HANDLE writePipe = nullptr;
    if (capturedOutput) {
        SECURITY_ATTRIBUTES pipeSecurity{sizeof(SECURITY_ATTRIBUTES), nullptr, TRUE};
        if (!CreatePipe(&readPipe, &writePipe, &pipeSecurity, 0) ||
            !SetHandleInformation(readPipe, HANDLE_FLAG_INHERIT, 0)) {
            if (readPipe) CloseHandle(readPipe);
            if (writePipe) CloseHandle(writePipe);
            return false;
        }
        si.dwFlags |= STARTF_USESTDHANDLES;
        si.hStdOutput = writePipe;
        si.hStdError = writePipe;
        si.hStdInput = nullptr;
    }

    PROCESS_INFORMATION pi{};
    std::wstring cmdBuffer = cmd;

    std::vector<unsigned char> attributeStorage;
    if (capturedOutput) {
        SIZE_T attributeBytes = 0;
        InitializeProcThreadAttributeList(nullptr, 1, 0, &attributeBytes);
        if (attributeBytes == 0) {
            CloseHandle(readPipe);
            CloseHandle(writePipe);
            return false;
        }
        attributeStorage.resize(attributeBytes);
        startupInfo.lpAttributeList = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(
            attributeStorage.data());
        if (!InitializeProcThreadAttributeList(
                startupInfo.lpAttributeList, 1, 0, &attributeBytes)) {
            CloseHandle(readPipe);
            CloseHandle(writePipe);
            return false;
        }

        // bInheritHandles must remain TRUE for STARTF_USESTDHANDLES, but the
        // attribute list turns inheritance into an explicit allow-list.
        HANDLE inheritedHandles[] = {writePipe};
        if (!UpdateProcThreadAttribute(
                startupInfo.lpAttributeList, 0,
                PROC_THREAD_ATTRIBUTE_HANDLE_LIST,
                inheritedHandles, sizeof(inheritedHandles), nullptr, nullptr)) {
            DeleteProcThreadAttributeList(startupInfo.lpAttributeList);
            startupInfo.lpAttributeList = nullptr;
            CloseHandle(readPipe);
            CloseHandle(writePipe);
            return false;
        }
        si.cb = sizeof(STARTUPINFOEXW);
    }

    BOOL ok = CreateProcessW(
        nullptr,
        cmdBuffer.data(),
        nullptr,
        nullptr,
        capturedOutput != nullptr,
        CREATE_NO_WINDOW | (capturedOutput ? EXTENDED_STARTUPINFO_PRESENT : 0),
        nullptr,
        nullptr,
        reinterpret_cast<LPSTARTUPINFOW>(&startupInfo),
        &pi
    );

    if (startupInfo.lpAttributeList) {
        DeleteProcThreadAttributeList(startupInfo.lpAttributeList);
        startupInfo.lpAttributeList = nullptr;
    }

    if (!ok) {
        if (readPipe) CloseHandle(readPipe);
        if (writePipe) CloseHandle(writePipe);
        return false;
    }

    CloseHandle(pi.hThread);
    pi.hThread = nullptr;

    if (writePipe) {
        CloseHandle(writePipe);
        writePipe = nullptr;
    }

    bool timedOut = false;
    bool outputTooLarge = false;
    bool pipeFailed = false;
    if (capturedOutput) {
        capturedOutput->clear();
        char buffer[4096];
        const auto appendOutput = [&](const char* data, DWORD bytes) {
            if (capturedOutput->size() + bytes > MaxCapturedOutputBytes) {
                outputTooLarge = true;
                return false;
            }
            capturedOutput->append(data, bytes);
            return true;
        };
        const ULONGLONG deadline = GetTickCount64() + 3000;
        for (;;) {
            DWORD available = 0;
            if (!PeekNamedPipe(readPipe, nullptr, 0, nullptr, &available, nullptr)) {
                DWORD currentExitCode = STILL_ACTIVE;
                if (!GetExitCodeProcess(pi.hProcess, &currentExitCode) || currentExitCode == STILL_ACTIVE) {
                    pipeFailed = true;
                    TerminateProcess(pi.hProcess, ERROR_BROKEN_PIPE);
                    WaitForSingleObject(pi.hProcess, 1000);
                }
                break;
            }
            while (available > 0) {
                DWORD bytesRead = 0;
                const DWORD requested = std::min<DWORD>(available, sizeof(buffer));
                if (!ReadFile(readPipe, buffer, requested, &bytesRead, nullptr) || bytesRead == 0) break;
                if (!appendOutput(buffer, bytesRead)) {
                    TerminateProcess(pi.hProcess, ERROR_BUFFER_OVERFLOW);
                    WaitForSingleObject(pi.hProcess, 1000);
                    break;
                }
                available -= bytesRead;
            }

            if (outputTooLarge) break;

            const ULONGLONG now = GetTickCount64();
            if (now >= deadline) {
                timedOut = true;
                TerminateProcess(pi.hProcess, ERROR_TIMEOUT);
                WaitForSingleObject(pi.hProcess, 1000);
                break;
            }
            const DWORD waitMs = static_cast<DWORD>((std::min)(
                deadline - now, static_cast<ULONGLONG>(50)));
            const DWORD processWait = WaitForSingleObject(pi.hProcess, waitMs);
            if (processWait == WAIT_OBJECT_0) break;
            if (processWait == WAIT_FAILED) {
                pipeFailed = true;
                TerminateProcess(pi.hProcess, GetLastError());
                WaitForSingleObject(pi.hProcess, 1000);
                break;
            }
        }

        if (!outputTooLarge) {
            // Drain only bytes already buffered. A blocking ReadFile can hang
            // forever if a descendant process retained the pipe handle.
            for (;;) {
                DWORD available = 0;
                if (!PeekNamedPipe(readPipe, nullptr, 0, nullptr, &available, nullptr) || available == 0) break;
                DWORD bytesRead = 0;
                const DWORD requested = (std::min)(available, static_cast<DWORD>(sizeof(buffer)));
                if (!ReadFile(readPipe, buffer, requested, &bytesRead, nullptr) || bytesRead == 0) break;
                if (!appendOutput(buffer, bytesRead)) break;
            }
        }
        CloseHandle(readPipe);
    }

    const DWORD waitResult = WaitForSingleObject(pi.hProcess, 3000);
    if (waitResult == WAIT_TIMEOUT) {
        timedOut = true;
        TerminateProcess(pi.hProcess, ERROR_TIMEOUT);
        WaitForSingleObject(pi.hProcess, 1000);
    }
    DWORD exitCode = 1;
    GetExitCodeProcess(pi.hProcess, &exitCode);
    CloseHandle(pi.hProcess);
    return !timedOut && !outputTooLarge && !pipeFailed && exitCode == 0;
}

std::optional<std::filesystem::path> currentExecutablePath() {
    wchar_t exePath[32768]{};
    const DWORD length = GetModuleFileNameW(
        nullptr, exePath, static_cast<DWORD>(std::size(exePath)));
    if (length == 0 || length >= std::size(exePath)) return std::nullopt;
    return std::filesystem::path(std::wstring(exePath, length));
}

std::optional<std::wstring> queryAutoStartTaskXml() {
    std::string output;
    // 优先查询当前用户隔离的任务计划 (Tools3000\Autorun for <User>)
    std::wstring taskName = tools3000::core::autostart::getAutoStartTaskName();
    std::wstring queryCmd = L"schtasks.exe /query /tn \"" + taskName + L"\" /xml";
    if (executeSilentCommand(queryCmd, &output) && !output.empty()) {
        return tools3000::core::WinUtils::utf8ToWstring(output);
    }

    return std::nullopt;
}

bool removeRegistryAutoStart() {
    if (tools3000::core::autostart::isTestMode()) {
        return true;
    }
    constexpr wchar_t keyPath[] = L"Software\\Microsoft\\Windows\\CurrentVersion\\Run";
    HKEY key = nullptr;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, keyPath, 0, KEY_SET_VALUE, &key) == ERROR_SUCCESS) {
        RegDeleteValueW(key, L"Tools3000");
        RegCloseKey(key);
    }
    return true;
}

bool setRegistryAutoStart(bool enabled) {
    if (tools3000::core::autostart::isTestMode()) {
        return true;
    }
    constexpr wchar_t keyPath[] = L"Software\\Microsoft\\Windows\\CurrentVersion\\Run";
    HKEY key = nullptr;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, keyPath, 0, nullptr, 0, KEY_SET_VALUE,
                        nullptr, &key, nullptr) != ERROR_SUCCESS) {
        return false;
    }

    LSTATUS status = ERROR_SUCCESS;
    if (enabled) {
        wchar_t exePath[32768]{};
        DWORD length = GetModuleFileNameW(nullptr, exePath, static_cast<DWORD>(std::size(exePath)));
        if (length == 0 || length >= std::size(exePath)) {
            RegCloseKey(key);
            return false;
        }
        std::wstring command = L"\"" + std::wstring(exePath, length) + L"\" --silent";
        status = RegSetValueExW(key, L"Tools3000", 0, REG_SZ,
                                reinterpret_cast<const BYTE*>(command.c_str()),
                                static_cast<DWORD>((command.size() + 1) * sizeof(wchar_t)));
    } else {
        status = RegDeleteValueW(key, L"Tools3000");
        if (status == ERROR_FILE_NOT_FOUND) status = ERROR_SUCCESS;
    }
    RegCloseKey(key);
    return status == ERROR_SUCCESS;
}

bool isAutoStartEnabled() {
    // 0. 测试模式全局熔断：避免单测产生对 Task Scheduler 或物理注册表的探测副作用
    if (tools3000::core::autostart::isTestMode()) {
        return tools3000::core::ConfigManager::instance().get<bool>("/general/autoStart", false);
    }

    const auto executable = currentExecutablePath();

    // 1. 计划任务检查
    if (executable) {
        tools3000::core::autostart::migrateLegacyTasksCOM(executable->wstring());
    }

    // 2. 原生 COM 接口精准检查
    if (executable && tools3000::core::autostart::isTaskRegisteredCOM(executable->wstring())) {
        return true;
    }

    // 3. 开发环境防篡改守卫：若当前为非生产二进制且系统存在已配置好的正式安装版，保持生效
    if (executable && tools3000::core::autostart::isNonProductionBinary(*executable)) {
        if (const auto prodExe = tools3000::core::autostart::getInstalledProductionExecutable()) {
            if (tools3000::core::autostart::isTaskRegisteredCOM(prodExe->wstring())) {
                return true;
            }
        }
    }

    // 4. 查询计划任务 XML 内容校验
    if (const auto taskXml = queryAutoStartTaskXml()) {
        if (executable && tools3000::core::autostart::taskTargetsExecutable(*taskXml, *executable)) {
            return true;
        }
        if (executable && tools3000::core::autostart::isNonProductionBinary(*executable)) {
            if (const auto prodExe = tools3000::core::autostart::getInstalledProductionExecutable()) {
                if (tools3000::core::autostart::taskTargetsExecutable(*taskXml, *prodExe)) {
                    return true;
                }
            }
            // 在非生产环境下，即便计划任务指向其他路径，也绝不将其物理篡改为非生产二进制
            return false;
        }

        LOG_WARN("AutoStart task points to a stale executable; repairing it for the current install.");
        if (setAutoStart(true)) {
            LOG_INFO("AutoStart task repaired successfully.");
            return true;
        }
        LOG_ERROR("AutoStart task repair failed.");
    }

    // 5. 兜底查询注册表 Run 键 (Tools3000)
    constexpr wchar_t keyPath[] = L"Software\\Microsoft\\Windows\\CurrentVersion\\Run";
    HKEY key = nullptr;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, keyPath, 0, KEY_QUERY_VALUE, &key) == ERROR_SUCCESS) {
        DWORD type = 0;
        LSTATUS status = RegQueryValueExW(key, L"Tools3000", nullptr, &type, nullptr, nullptr);
        RegCloseKey(key);
        if (status == ERROR_SUCCESS) return true;
    }
    return false;
}

// ── 开机自启状态原子缓存（启动预热 + 后台快读 <1µs） ─────────────────────
std::atomic<bool> s_autoStartCached{false};
std::atomic<bool> s_autoStartCacheValid{false};
std::mutex s_autoStartMutex;

bool isAutoStartEnabled();

bool getCachedAutoStart(bool forceRefresh = false) {
    if (!forceRefresh && s_autoStartCacheValid.load(std::memory_order_acquire)) {
        return s_autoStartCached.load(std::memory_order_relaxed);
    }
    std::lock_guard<std::mutex> lock(s_autoStartMutex);
    if (!forceRefresh && s_autoStartCacheValid.load(std::memory_order_relaxed)) {
        return s_autoStartCached.load(std::memory_order_relaxed);
    }
    const bool enabled = isAutoStartEnabled();
    s_autoStartCached.store(enabled, std::memory_order_relaxed);
    s_autoStartCacheValid.store(true, std::memory_order_release);
    return enabled;
}

void updateAutoStartCache(bool enabled) {
    s_autoStartCached.store(enabled, std::memory_order_relaxed);
    s_autoStartCacheValid.store(true, std::memory_order_release);
}

} // namespace

bool setAutoStartImpl(bool enabled) {
    if (tools3000::core::autostart::isTestMode()) {
        LOG_INFO("AutoStart change skipped because test mode (TOOLS3000_TEST_MODE) is active.");
        return true;
    }

    const auto executable = currentExecutablePath();
    if (!executable) {
        return setRegistryAutoStart(enabled);
    }

    const std::wstring exeStr = executable->wstring();
    const std::wstring exeDir = executable->parent_path().wstring();
    const std::wstring userTaskName = tools3000::core::autostart::getFullTaskPath();

    if (enabled) {
        removeRegistryAutoStart();

        // 1. 如果已是管理员，直接原生 COM API 注册
        if (tools3000::core::WinUtils::isCurrentProcessElevated()) {
            if (tools3000::core::autostart::registerTaskCOM(exeStr)) {
                LOG_INFO("AutoStart Task Scheduler task '{}' created via native TaskScheduler COM API.",
                         tools3000::core::WinUtils::wstringToUtf8(userTaskName));
                return true;
            }
        } else {
            // 2. 如果是普通权限，通过 ShellExecuteEx 请求提权执行 --register-autostart
            SHELLEXECUTEINFOW sei = {sizeof(sei)};
            sei.fMask = SEE_MASK_NOCLOSEPROCESS;
            sei.lpVerb = L"runas";
            sei.lpFile = exeStr.c_str();
            sei.lpParameters = L"--register-autostart";
            sei.lpDirectory = exeDir.c_str();
            sei.nShow = SW_HIDE;
            if (ShellExecuteExW(&sei)) {
                if (sei.hProcess) {
                    WaitForSingleObject(sei.hProcess, 5000);
                    DWORD exitCode = 1;
                    GetExitCodeProcess(sei.hProcess, &exitCode);
                    CloseHandle(sei.hProcess);
                    if (exitCode == 0) {
                        LOG_INFO("AutoStart Task Scheduler task '{}' registered via elevated helper.",
                                 tools3000::core::WinUtils::wstringToUtf8(userTaskName));
                        return true;
                    }
                }
            }
        }

        // 3. 兜底方案：注册表 HKCU Run 键
        LOG_WARN("Task Scheduler autostart failed/denied, falling back to Registry Run key.");
        return setRegistryAutoStart(true);
    } else {
        if (tools3000::core::WinUtils::isCurrentProcessElevated()) {
            tools3000::core::autostart::unregisterTaskCOM();
        } else {
            SHELLEXECUTEINFOW sei = {sizeof(sei)};
            sei.fMask = SEE_MASK_NOCLOSEPROCESS;
            sei.lpVerb = L"runas";
            sei.lpFile = exeStr.c_str();
            sei.lpParameters = L"--unregister-autostart";
            sei.lpDirectory = exeDir.c_str();
            sei.nShow = SW_HIDE;
            if (ShellExecuteExW(&sei)) {
                if (sei.hProcess) {
                    WaitForSingleObject(sei.hProcess, 5000);
                    CloseHandle(sei.hProcess);
                }
            }
        }
        removeRegistryAutoStart();
        LOG_INFO("AutoStart disabled idempotently (Task Scheduler and Registry entries cleared).");
        return true;
    }
}

bool setAutoStart(bool enabled) {
    const bool ok = setAutoStartImpl(enabled);
    if (ok) {
        updateAutoStartCache(enabled);
    }
    return ok;
}

void applyLogLevel(const std::string& level) {
    using spdlog::level::level_enum;
    static const std::unordered_map<std::string, level_enum> levels = {
        {"trace", level_enum::trace}, {"debug", level_enum::debug},
        {"info", level_enum::info}, {"warn", level_enum::warn},
        {"error", level_enum::err},
    };
    if (const auto it = levels.find(level); it != levels.end()) {
        tools3000::core::Logger::setLevel(it->second);
    }
}

std::string canonicalHotkeyName(std::string name) {
    static const std::unordered_map<std::string, std::string> aliases = {
        {"Recording", "Record"}, {"capture", "Screenshot"},
        {"recording", "Record"}, {"ocr", "OCR"},
        {"gesturePause", "Pause Gestures"},
    };
    if (const auto it = aliases.find(name); it != aliases.end()) return it->second;
    return name;
}

}  // namespace

namespace tools3000::core {

MessageBridge& MessageBridge::instance() {
    static MessageBridge inst;
    return inst;
}

void MessageBridge::registerHandler(const std::string& method, MessageHandler handler) {
    auto slot = std::make_shared<HandlerSlot>();
    slot->handler = std::move(handler);
    std::shared_ptr<HandlerSlot> replaced;
    {
        std::unique_lock lock(m_mutex);
        if (auto it = m_handlers.find(method); it != m_handlers.end()) {
            replaced = std::move(it->second);
            it->second = std::move(slot);
        } else {
            m_handlers.emplace(method, std::move(slot));
        }
    }
    if (replaced) retireSlots({std::move(replaced)});
    LOG_TRACE("注册 IPC 处理器: method={}", method);
}

void MessageBridge::retireSlots(std::vector<std::shared_ptr<HandlerSlot>> slots,
                                bool boundedForShutdown) {
    // Intentionally leaked only for pathological handlers that are still
    // executing during process shutdown. Destroying their std::function (or a
    // plugin DLL) while its code is on another thread would be a UAF.
    static auto* quarantinedMutex = new std::mutex();
    static auto* quarantinedSlots = new std::vector<std::shared_ptr<HandlerSlot>>();
    for (const auto& slot : slots) {
        std::unique_lock lock(slot->mutex);
        slot->accepting = false;
        if (boundedForShutdown &&
            !slot->idle.wait_for(lock, std::chrono::seconds(2),
                                 [&slot]() { return slot->activeCalls == 0; })) {
            LOG_CRITICAL("IPC handler 在关闭期限内未返回，已隔离保留以避免卸载 UAF: activeCalls={}",
                         slot->activeCalls);
            lock.unlock();
            std::lock_guard quarantineLock(*quarantinedMutex);
            quarantinedSlots->push_back(slot);
            continue;
        }
        if (!boundedForShutdown) {
            slot->idle.wait(lock, [&slot]() { return slot->activeCalls == 0; });
        }
        slot->handler = nullptr;
    }
}

void MessageBridge::unregisterHandler(const std::string& method) {
    std::shared_ptr<HandlerSlot> removed;
    {
        std::unique_lock lock(m_mutex);
        if (const auto it = m_handlers.find(method); it != m_handlers.end()) {
            removed = std::move(it->second);
            m_handlers.erase(it);
        }
    }
    if (removed) retireSlots({std::move(removed)}, true);
}

size_t MessageBridge::unregisterHandlersByPrefix(const std::string& prefix) {
    std::vector<std::shared_ptr<HandlerSlot>> removed;
    {
        std::unique_lock lock(m_mutex);
        for (auto it = m_handlers.begin(); it != m_handlers.end();) {
            if (it->first.starts_with(prefix)) {
                removed.push_back(std::move(it->second));
                it = m_handlers.erase(it);
            } else {
                ++it;
            }
        }
    }
    const size_t count = removed.size();
    retireSlots(std::move(removed), true);
    if (count > 0) LOG_DEBUG("注销 IPC 命名空间: prefix={}, count={}", prefix, count);
    return count;
}

std::string MessageBridge::handleMessage(const std::string& messageJson) {
    TraceId::Scope scope;
    int id = 0;
    if (messageJson.size() > MaxBridgeMessageBytes) {
        LOG_WARN("拒绝过大的 IPC 消息: {} bytes", messageJson.size());
        return bridgeErrorResponse(id, -32600, "Request exceeds 1 MiB limit");
    }
    try {
        auto request = json::parse(messageJson);
        id = request.value("id", 0);
        std::string method = request.value("method", "");
        json params = request.value("params", json::object());

        LOG_DEBUG("收到前端消息: id={}, method={}", id, method);

        std::shared_ptr<HandlerSlot> slot;
        {
            std::shared_lock lock(m_mutex);
            auto it = m_handlers.find(method);
            if (it != m_handlers.end()) slot = it->second;
        }
        bool acquired = false;
        if (slot) {
            std::lock_guard lock(slot->mutex);
            if (slot->accepting && slot->handler) {
                ++slot->activeCalls;
                acquired = true;
            }
        }
        if (!acquired) {
            LOG_WARN("未知的 IPC 方法: {}", method);
            json response = {
                {"id", id},
                {"error", {{"code", -32601}, {"message", "Method not found: " + method}}}
            };
            return response.dump();
        }

        const auto start = std::chrono::steady_clock::now();
        json result;
        try {
            result = slot->handler(params);
        } catch (...) {
            std::lock_guard lock(slot->mutex);
            if (--slot->activeCalls == 0) slot->idle.notify_all();
            throw;
        }
        {
            std::lock_guard lock(slot->mutex);
            if (--slot->activeCalls == 0) slot->idle.notify_all();
        }
        const auto elapsedUs = std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::steady_clock::now() - start).count();
        LOG_DEBUG("IPC 响应完成: method={}, id={}, 耗时={}us", method, id, elapsedUs);

        json response = {
            {"id", id},
            {"result", result}
        };
        return response.dump();
    } catch (const std::exception& e) {
        LOG_ERROR("IPC 处理器异常: id={}, error={}", id, e.what());
        json response = {
            {"id", id},
            {"error", {{"code", -32603}, {"message", std::string("Internal error: ") + e.what()}}}
        };
        return response.dump();
    } catch (...) {
        LOG_ERROR("IPC 处理器未知异常: id={}", id);
        json response = {
            {"id", id},
            {"error", {{"code", -32603}, {"message", "Internal error: unknown exception"}}}
        };
        return response.dump();
    }
}

// ── 异步方法线程池 ──────────────────────────────────────────────────────────
//
// 队列里只保存原始消息文本和响应回调，处理器是任务真正执行时才从 m_handlers
// 查找的。因此排队中的任务不会持有插件 DLL 内的 std::function，插件卸载不会
// 留下指向已卸载模块的悬空调用。

namespace {

constexpr size_t AsyncWorkerCount = 4;

/// 队列上限。搜索场景下堆积几乎都来自连续击键，因此过载时丢弃最旧的一条，
/// 并立刻给它回一个错误响应，避免前端 Promise 悬挂到超时。
constexpr size_t MaxQueuedAsyncJobs = 64;
constexpr auto DefaultAsyncJobTimeout = std::chrono::seconds(30);
// 全文搜索会真实读取并解压大量文档。前端已经为 search.query 保留 30 分钟，
// 后端不能再用普通 IPC 的 30 秒熔断提前返回空结果。
constexpr auto SearchQueryAsyncJobTimeout = std::chrono::minutes(30);

std::mutex g_workerPoolMutex;

}  // namespace

struct MessageBridge::WorkerPool {
    struct Completion {
        Completion(int requestId, AsyncResponder callback)
            : id(requestId), responder(std::move(callback)) {}

        int id = 0;
        AsyncResponder responder;
        std::atomic<bool> responded{false};
    };

    struct Job {
        std::string message;
        std::shared_ptr<Completion> completion;
        std::chrono::steady_clock::duration timeout = DefaultAsyncJobTimeout;
    };

    struct WorkerState {
        bool active = false;
        bool timedOut = false;
        std::chrono::steady_clock::time_point startedAt{};
        std::chrono::steady_clock::duration timeout = DefaultAsyncJobTimeout;
        std::shared_ptr<Completion> completion;
    };

    std::vector<std::thread> threads;
    std::vector<std::unique_ptr<WorkerState>> workerStates;
    std::thread watchdog;
    std::deque<Job> jobs;
    std::mutex mutex;
    std::condition_variable cv;
    std::condition_variable watchdogCv;
    bool stopping = false;
    bool circuitOpen = false;

    static void respondOnce(const std::shared_ptr<Completion>& completion,
                            std::string response,
                            const char* context) noexcept {
        if (!completion || !completion->responder ||
            completion->responded.exchange(true, std::memory_order_acq_rel)) {
            return;
        }
        try {
            completion->responder(std::move(response));
        } catch (const std::exception& e) {
            LOG_ERROR("异步 IPC 响应回调异常: context={}, error={}", context, e.what());
        } catch (...) {
            LOG_ERROR("异步 IPC 响应回调未知异常: context={}", context);
        }
    }
};

MessageBridge::WorkerPool& MessageBridge::ensureWorkerPool() {
    std::lock_guard guard(g_workerPoolMutex);
    if (m_workerPool) return *m_workerPool;

    auto* pool = new WorkerPool();
    pool->threads.reserve(AsyncWorkerCount);
    pool->workerStates.reserve(AsyncWorkerCount);
    for (size_t i = 0; i < AsyncWorkerCount; ++i) {
        pool->workerStates.push_back(std::make_unique<WorkerPool::WorkerState>());
        auto* workerState = pool->workerStates.back().get();
        pool->threads.emplace_back([pool, workerState]() {
            for (;;) {
                WorkerPool::Job job;
                {
                    std::unique_lock lock(pool->mutex);
                    pool->cv.wait(lock, [pool] { return pool->stopping || !pool->jobs.empty(); });
                    if (pool->jobs.empty()) return;  // stopping 且队列已清空
                    job = std::move(pool->jobs.front());
                    pool->jobs.pop_front();
                    workerState->active = true;
                    workerState->timedOut = false;
                    workerState->startedAt = std::chrono::steady_clock::now();
                    workerState->timeout = job.timeout;
                    workerState->completion = job.completion;
                }
                std::string response = MessageBridge::instance().handleMessage(job.message);
                WorkerPool::respondOnce(job.completion, std::move(response), "worker");
                {
                    std::lock_guard lock(pool->mutex);
                    workerState->active = false;
                    workerState->timedOut = false;
                    workerState->completion.reset();
                }
                pool->watchdogCv.notify_one();
            }
        });
    }
    pool->watchdog = std::thread([pool]() {
        std::unique_lock lock(pool->mutex);
        while (!pool->stopping) {
            pool->watchdogCv.wait_for(lock, std::chrono::seconds(1),
                                      [pool] { return pool->stopping; });
            if (pool->stopping) break;

            const auto now = std::chrono::steady_clock::now();
            struct TimedOutJob {
                std::shared_ptr<WorkerPool::Completion> completion;
                std::chrono::steady_clock::duration timeout;
            };
            std::vector<TimedOutJob> timedOut;
            size_t stalledWorkers = 0;
            for (const auto& state : pool->workerStates) {
                if (!state->active || now - state->startedAt < state->timeout) continue;
                ++stalledWorkers;
                if (!state->timedOut) {
                    state->timedOut = true;
                    timedOut.push_back({state->completion, state->timeout});
                }
            }

            std::deque<WorkerPool::Job> rejected;
            if (stalledWorkers == pool->workerStates.size()) {
                if (!pool->circuitOpen) {
                    pool->circuitOpen = true;
                    rejected.swap(pool->jobs);
                }
            } else if (pool->circuitOpen) {
                pool->circuitOpen = false;
                LOG_WARN("异步 IPC 熔断器已恢复: availableWorkers={}",
                         pool->workerStates.size() - stalledWorkers);
            }

            lock.unlock();
            for (const auto& job : timedOut) {
                LOG_ERROR("异步 IPC handler 超时: requestId={}, timeoutSeconds={}",
                          job.completion ? job.completion->id : 0,
                          std::chrono::duration_cast<std::chrono::seconds>(job.timeout).count());
                const int id = job.completion ? job.completion->id : 0;
                WorkerPool::respondOnce(
                    job.completion,
                    bridgeErrorResponse(id, -32001, "Async handler timed out"),
                    "watchdog timeout");
            }
            if (!rejected.empty()) {
                LOG_CRITICAL("异步 IPC 所有 worker 均超时，熔断并拒绝排队请求: count={}",
                             rejected.size());
                for (auto& job : rejected) {
                    const int id = job.completion ? job.completion->id : 0;
                    WorkerPool::respondOnce(
                        job.completion,
                        bridgeErrorResponse(id, -32002, "Async bridge circuit is open"),
                        "watchdog circuit breaker");
                }
            }
            lock.lock();
        }
    });
    m_workerPool = pool;
    LOG_DEBUG("异步 IPC 线程池已启动: threads={}", AsyncWorkerCount);
    return *pool;
}

void MessageBridge::shutdownWorkerPool() {
    WorkerPool* pool = nullptr;
    {
        std::lock_guard guard(g_workerPoolMutex);
        pool = m_workerPool;
        m_workerPool = nullptr;
    }
    if (!pool) return;

    std::deque<WorkerPool::Job> abandoned;
    {
        std::lock_guard lock(pool->mutex);
        pool->stopping = true;
        abandoned.swap(pool->jobs);
    }
    pool->cv.notify_all();
    pool->watchdogCv.notify_all();
    if (pool->watchdog.joinable()) pool->watchdog.join();

    // 让仍在排队的请求立刻失败，而不是让前端等到超时。
    for (auto& job : abandoned) {
        const int id = job.completion ? job.completion->id : 0;
        WorkerPool::respondOnce(
            job.completion,
            bridgeErrorResponse(id, -32000, "Bridge is shutting down"),
            "shutdown queue");
    }

    bool leakedForStalledWorkers = false;
    for (size_t i = 0; i < pool->threads.size(); ++i) {
        auto& thread = pool->threads[i];
        if (!thread.joinable()) continue;

        std::shared_ptr<WorkerPool::Completion> activeCompletion;
        {
            std::lock_guard lock(pool->mutex);
            if (pool->workerStates[i]->active) {
                activeCompletion = pool->workerStates[i]->completion;
            }
        }
        if (activeCompletion) {
            const int id = activeCompletion->id;
            WorkerPool::respondOnce(
                activeCompletion,
                bridgeErrorResponse(id, -32000, "Bridge shut down while handler was running"),
                "shutdown active worker");
            // Standard C++ cannot safely terminate a handler executing inside
            // a third-party DLL. Detach it and intentionally retain the pool
            // state so a late return cannot access freed memory.
            thread.detach();
            leakedForStalledWorkers = true;
        } else {
            thread.join();
        }
    }

    if (leakedForStalledWorkers) {
        LOG_CRITICAL("异步 IPC 关闭时仍有 handler 卡住；已脱离线程并保留池状态以避免 join 死锁/UAF");
    } else {
        delete pool;
        LOG_DEBUG("异步 IPC 线程池已关闭");
    }
}

void MessageBridge::markMethodAsync(const std::string& method) {
    {
        std::unique_lock lock(m_mutex);
        m_asyncMethods.insert(method);
    }
    ensureWorkerPool();
    LOG_DEBUG("IPC 方法已标记为异步执行: {}", method);
}

void MessageBridge::handleMessageAsync(const std::string& messageJson, AsyncResponder responder) {
    if (!responder) return;

    if (messageJson.size() > MaxBridgeMessageBytes) {
        LOG_WARN("拒绝过大的异步 IPC 消息: {} bytes", messageJson.size());
        responder(bridgeErrorResponse(0, -32600, "Request exceeds 1 MiB limit"));
        return;
    }

    int id = 0;
    bool runAsync = false;
    std::string method;
    try {
        auto request = json::parse(messageJson);
        id = request.value("id", 0);
        method = request.value("method", "");
        std::shared_lock lock(m_mutex);
        runAsync = m_asyncMethods.find(method) != m_asyncMethods.end();
    } catch (...) {
        // 解析失败时交给同步路径，由它统一产出格式正确的错误响应。
        runAsync = false;
    }

    if (!runAsync) {
        responder(handleMessage(messageJson));
        return;
    }

    auto& pool = ensureWorkerPool();
    auto completion = std::make_shared<WorkerPool::Completion>(id, std::move(responder));
    std::shared_ptr<WorkerPool::Completion> rejected;
    std::shared_ptr<WorkerPool::Completion> evicted;
    std::string rejectionMessage;
    {
        std::lock_guard lock(pool.mutex);
        if (pool.stopping) {
            rejected = completion;
            rejectionMessage = "Bridge is shutting down";
        } else if (pool.circuitOpen) {
            rejected = completion;
            rejectionMessage = "Async bridge circuit is open";
        } else {
            if (pool.jobs.size() >= MaxQueuedAsyncJobs) {
                evicted = std::move(pool.jobs.front().completion);
                pool.jobs.pop_front();
            }
            const auto timeout = method == "search.query"
                ? SearchQueryAsyncJobTimeout
                : DefaultAsyncJobTimeout;
            pool.jobs.push_back(WorkerPool::Job{messageJson, completion, timeout});
        }
    }
    if (rejected) {
        WorkerPool::respondOnce(
            rejected,
            bridgeErrorResponse(id, pool.stopping ? -32000 : -32002, rejectionMessage),
            "enqueue rejected");
        return;
    }
    pool.cv.notify_one();

    if (evicted) {
        LOG_WARN("异步 IPC 队列过载, 丢弃最旧的待处理请求");
        WorkerPool::respondOnce(
            evicted,
            bridgeErrorResponse(evicted->id, -32000,
                                "Request dropped: bridge queue overloaded"),
            "queue eviction");
    }
}

bool MessageBridge::hasHandler(const std::string& method) const {
    std::shared_lock lock(m_mutex);
    auto it = m_handlers.find(method);
    if (it == m_handlers.end()) return false;
    std::lock_guard slotLock(it->second->mutex);
    return it->second->accepting && static_cast<bool>(it->second->handler);
}

json MessageBridge::invokeHandler(const std::string& method, const json& params) {
    std::shared_ptr<HandlerSlot> slot;
    {
        std::shared_lock lock(m_mutex);
        auto it = m_handlers.find(method);
        if (it != m_handlers.end()) slot = it->second;
    }
    if (!slot) return {{"error", "handler not found"}};
    {
        std::lock_guard slotLock(slot->mutex);
        if (!slot->accepting || !slot->handler) return {{"error", "handler not accepting"}};
        ++slot->activeCalls;
    }
    json result;
    try {
        result = slot->handler(params);
    } catch (const std::exception& e) {
        std::lock_guard slotLock(slot->mutex);
        if (--slot->activeCalls == 0) slot->idle.notify_all();
        return {{"error", e.what()}};
    } catch (...) {
        std::lock_guard slotLock(slot->mutex);
        if (--slot->activeCalls == 0) slot->idle.notify_all();
        return {{"error", "unknown error"}};
    }
    {
        std::lock_guard slotLock(slot->mutex);
        if (--slot->activeCalls == 0) slot->idle.notify_all();
    }
    return result;
}

void MessageBridge::setEventPusher(EventPusher pusher) {
    registerEventPusher("default", std::move(pusher));
}

void MessageBridge::registerEventPusher(const std::string& name, EventPusher pusher) {
    std::unique_lock lock(m_mutex);
    if (pusher) {
        m_eventPushers[name] = std::move(pusher);
        LOG_DEBUG("事件推送器已设置");
    } else {
        m_eventPushers.erase(name);
    }
}

void MessageBridge::unregisterEventPusher(const std::string& name) {
    std::unique_lock lock(m_mutex);
    m_eventPushers.erase(name);
}

void MessageBridge::clearHandlers() {
    // Stop async dispatch first: queued jobs look their handler up at execution
    // time, so draining them before clearing m_handlers avoids spurious
    // "method not found" responses during shutdown.
    shutdownWorkerPool();

    // std::function destructors may release objects whose cleanup re-enters the
    // bridge. Move callbacks out while locked, then destroy them without
    // holding m_mutex to avoid shutdown deadlocks.
    std::vector<std::shared_ptr<HandlerSlot>> handlers;
    std::unordered_map<std::string, EventPusher> pushers;
    {
        std::unique_lock lock(m_mutex);
        handlers.reserve(m_handlers.size());
        for (auto& [_, slot] : m_handlers) handlers.push_back(std::move(slot));
        m_handlers.clear();
        m_asyncMethods.clear();
        pushers.swap(m_eventPushers);
    }
    pushers.clear();
    retireSlots(std::move(handlers), true);
    LOG_INFO("IPC 处理器与事件推送器已清空");
}

void MessageBridge::pushEvent(const std::string& eventName, const json& data) {
    std::vector<EventPusher> pushers;
    {
        std::shared_lock lock(m_mutex);
        pushers.reserve(m_eventPushers.size());
        for (const auto& [_, p] : m_eventPushers) {
            if (p) pushers.push_back(p);
        }
    }
    for (const auto& pusher : pushers) {
        try {
            pusher(eventName, data);
        } catch (...) {
        }
    }
    LOG_TRACE("推送事件到前端: event={}", eventName);
}

namespace {

struct MarketplaceItem {
    std::string id;
    std::string name;
    std::string nameEn;
    std::string version;
    std::string author;
    std::string description;
    std::string descriptionEn;
    std::string category;
    uint32_t abiVersion;
    std::vector<std::string> capabilities;
    std::vector<std::string> permissions;
    std::string downloadUrl;
    bool featured;
};

const std::vector<MarketplaceItem>& getMarketplaceCatalog() {
    static const std::vector<MarketplaceItem> catalog = {
        {
            "ai_assistant",
            "AI 悬浮助手",
            "AI Float Assistant",
            "1.2.0",
            "Tools3000 Team",
            "全局快捷划词翻译、智能解释、代码优化与文本润色悬浮窗",
            "Global shortcut word translation, smart explanation, and code refactor float window",
            "ai",
            1,
            {"floating-window", "ai-chat", "text-selection", "api-bridge"},
            {"network", "clipboard", "selection"},
            "https://raw.githubusercontent.com/yuan278501381/Tools3000/main/marketplace/ai_assistant.zip",
            true
        },
        {
            "color_picker",
            "高级屏幕取色与调色板",
            "Advanced Color Picker",
            "1.0.5",
            "Tools3000 Team",
            "像素级放大镜精准取色，支持 HEX/RGB/HSL/CMYK 一键复制与调色板收藏",
            "Pixel magnifier precision color picking with HEX/RGB/HSL/CMYK copying and palettes",
            "utility",
            1,
            {"screen-magnifier", "palette", "clipboard-copy"},
            {"screen-capture", "clipboard"},
            "https://raw.githubusercontent.com/yuan278501381/Tools3000/main/marketplace/color_picker.zip",
            true
        },
        {
            "clipboard_manager",
            "剪贴板历史与灵感库",
            "Clipboard Manager Pro",
            "1.1.0",
            "Tools3000 Team",
            "无限剪贴板历史记录、富文本/图片分类检索与常用文本置顶收藏",
            "Unlimited clipboard history, rich text/image search and pinned snippets",
            "productivity",
            1,
            {"clipboard-history", "snippet-manager", "search"},
            {"clipboard", "storage"},
            "https://raw.githubusercontent.com/yuan278501381/Tools3000/main/marketplace/clipboard_manager.zip",
            false
        },
        {
            "markdown_preview",
            "Markdown 桌面速览",
            "Markdown Quick Viewer",
            "1.0.1",
            "Community Contributor",
            "单按空格快速预览 .md 与代码文件，支持 GitHub 风格渲染与数学公式",
            "Quick spacebar preview for markdown and code files with LaTeX & KaTeX support",
            "productivity",
            1,
            {"quick-look", "markdown-render", "mathjax"},
            {"file-read"},
            "https://raw.githubusercontent.com/yuan278501381/Tools3000/main/marketplace/markdown_preview.zip",
            false
        }
    };
    return catalog;
}

bool isCoreBuiltinPlugin(const std::string& id) {
    return id == "gesture" || id == "capture" || id == "search" ||
           id == "keycast" || id == "dialogenhancer" || id == "dialog_enhancer" ||
           id == "spotlight" || id == "remote_boost" || id == "remote";
}

bool isExtensionInstalled(const std::string& id) {
    if (isCoreBuiltinPlugin(id)) return false;
    auto& config = ConfigManager::instance();
    auto installedList = config.get<std::vector<std::string>>("/plugins/installedExtensions", {});
    return std::find(installedList.begin(), installedList.end(), id) != installedList.end();
}

void markExtensionInstalled(const std::string& id, bool installed) {
    if (isCoreBuiltinPlugin(id)) return;
    auto& config = ConfigManager::instance();
    auto installedList = config.get<std::vector<std::string>>("/plugins/installedExtensions", {});
    auto it = std::find(installedList.begin(), installedList.end(), id);
    if (installed) {
        if (it == installedList.end()) {
            installedList.push_back(id);
            config.set("/plugins/installedExtensions", installedList);
            config.set("/plugins/" + id + "/enabled", true);
        }
    } else {
        if (it != installedList.end()) {
            installedList.erase(it);
            config.set("/plugins/installedExtensions", installedList);
        }
        config.set("/plugins/" + id + "/enabled", false);
    }
}

void releasePluginResourcesAndTrim() {
    tools3000::core::WinUtils::trimWorkingSet();
}

}  // namespace

void MessageBridge::registerBuiltinHandlers() {
    for (const char* method : {
             "plugins.setEnabled",
             "plugins.getAll",
             "plugins.install",
             "plugins.uninstall",
             "plugins.getMarketplace",
             "app.restart",
             "app.restartElevated",
             "app.restartDemoted",
             "app.trimMemory",
             "general.getSettings",
             "autoStart.set",
         }) {
        markMethodAsync(method);
    }

    // 后台异步预热开机自启状态原子缓存，确保后续查询耗时降至 <1µs 并杜绝主线程卡顿
    static std::once_flag s_warmupOnce;
    std::call_once(s_warmupOnce, []() {
        std::thread([]() {
            getCachedAutoStart(true);
        }).detach();
    });

    registerHandler("plugins.getAll", [](const json&) -> json {
        auto statuses = PluginManager::instance().getPluginStatuses();
        json plugins = json::array();
        std::unordered_set<std::string> existingIds;
        for (const auto& plugin : statuses) {
            existingIds.insert(plugin.id);
            const bool isExt = !isCoreBuiltinPlugin(plugin.id);
            plugins.push_back({
                {"id", plugin.id},
                {"name", plugin.name},
                {"version", plugin.version},
                {"fileName", plugin.fileName},
                {"abiVersion", plugin.abiVersion},
                {"capabilities", plugin.capabilities},
                {"permissions", plugin.permissions},
                {"executionModel", plugin.executionModel},
                {"enabled", plugin.enabled},
                {"active", plugin.active},
                {"restartRequired", plugin.restartRequired},
                {"state", plugin.state},
                {"error", plugin.error},
                {"isExtension", isExt}
            });
        }

        // 内置核心模块：鼠标演示与特效 (spotlight)
        const bool spotlightEnabled = ConfigManager::instance().has("/plugins/spotlight/enabled")
            ? ConfigManager::instance().get<bool>("/plugins/spotlight/enabled", true)
            : ConfigManager::instance().get<bool>("/spotlight/enabled", true);
        plugins.push_back({
            {"id", "spotlight"},
            {"name", "鼠标演示与特效"},
            {"version", tools3000::version::String},
            {"fileName", "Tools3000.exe"},
            {"abiVersion", 1},
            {"capabilities", {"spotlight", "click-ripple", "mouse-trail"}},
            {"permissions", {"low-level-mouse-hook", "direct2d-overlay"}},
            {"executionModel", "trusted-native-in-process"},
            {"enabled", spotlightEnabled},
            {"active", spotlightEnabled},
            {"restartRequired", false},
            {"state", spotlightEnabled ? "running" : "disabled"},
            {"error", ""},
            {"isExtension", false}
        });
        existingIds.insert("spotlight");

        // 内置核心模块：远程协助增强 (remote_boost)
        const auto remoteSettings = tools3000::core::RemoteMasterEngine::instance().getSettings();
        const bool remoteEnabled = ConfigManager::instance().has("/plugins/remote_boost/enabled")
            ? ConfigManager::instance().get<bool>("/plugins/remote_boost/enabled", remoteSettings.enabled)
            : (ConfigManager::instance().has("/plugins/remote/enabled")
                ? ConfigManager::instance().get<bool>("/plugins/remote/enabled", remoteSettings.enabled)
                : remoteSettings.enabled);
        plugins.push_back({
            {"id", "remote_boost"},
            {"name", "远程协助增强"},
            {"version", tools3000::version::String},
            {"fileName", "Tools3000.exe"},
            {"abiVersion", 1},
            {"capabilities", {"remote-tunnel", "hotkey-passthrough", "ime-sanitizer"}},
            {"permissions", {"low-level-keyboard-hook", "window-event-hook", "input-synthesis"}},
            {"executionModel", "trusted-native-in-process"},
            {"enabled", remoteEnabled},
            {"active", remoteEnabled},
            {"restartRequired", false},
            {"state", remoteEnabled ? "running" : "disabled"},
            {"error", ""},
            {"isExtension", false}
        });
        existingIds.insert("remote_boost");

        // 合并已安装的扩展插件
        auto& config = ConfigManager::instance();
        auto installedList = config.get<std::vector<std::string>>("/plugins/installedExtensions", {});
        const auto& catalog = getMarketplaceCatalog();
        for (const auto& item : catalog) {
            if (std::find(installedList.begin(), installedList.end(), item.id) != installedList.end() &&
                existingIds.find(item.id) == existingIds.end()) {
                plugins.push_back({
                    {"id", item.id},
                    {"name", item.name},
                    {"version", item.version},
                    {"fileName", "Plugin_" + item.id + ".dll"},
                    {"abiVersion", item.abiVersion},
                    {"capabilities", item.capabilities},
                    {"permissions", item.permissions},
                    {"executionModel", "trusted-native-in-process"},
                    {"enabled", false},
                    {"active", false},
                    {"restartRequired", false},
                    {"state", "unavailable"},
                    {"error", "扩展包尚未安装；旧版仅记录了目录状态"},
                    {"isExtension", true}
                });
            }
        }
        return plugins;
    });

    registerHandler("plugins.setEnabled", [](const json& params) -> json {
        if (!params.is_object() || !params.contains("id") || !params["id"].is_string() ||
            !params.contains("enabled") || !params["enabled"].is_boolean()) {
            return {{"success", false}, {"error", "id and enabled are required"}};
        }
        const std::string id = params["id"].get<std::string>();
        const bool enabled = params["enabled"].get<bool>();

        if (id == "spotlight") {
            ConfigManager::instance().set<bool>("/spotlight/enabled", enabled);
            ConfigManager::instance().set<bool>("/plugins/spotlight/enabled", enabled);
            EventBus::instance().publish(SpotlightStateChangedEvent{enabled});
            MessageBridge::instance().pushEvent("spotlight:stateChanged", {{"enabled", enabled}});
            MessageBridge::instance().pushEvent("plugins:changed", {{"id", "spotlight"}, {"enabled", enabled}});
            releasePluginResourcesAndTrim();
            return {{"success", true}, {"restartRequired", false}};
        }

        if (id == "remote_boost" || id == "remote") {
            auto s = tools3000::core::RemoteMasterEngine::instance().getSettings();
            s.enabled = enabled;
            tools3000::core::RemoteMasterEngine::instance().updateSettings(s);
            ConfigManager::instance().set<bool>("/plugins/remote_boost/enabled", enabled);
            ConfigManager::instance().set<bool>("/plugins/remote/enabled", enabled);
            MessageBridge::instance().pushEvent("remote:stateChanged", {{"enabled", enabled}});
            MessageBridge::instance().pushEvent("plugins:changed", {{"id", "remote_boost"}, {"enabled", enabled}});
            releasePluginResourcesAndTrim();
            return {{"success", true}, {"restartRequired", false}};
        }

        if (id == "dialogenhancer" || id == "dialog_enhancer") {
            const std::string normalizedId = "dialogenhancer";
            ConfigManager::instance().set<bool>("/dialog/enabled", enabled);
            ConfigManager::instance().set<bool>("/plugins/dialogenhancer/enabled", enabled);
            ConfigManager::instance().set<bool>("/plugins/dialog_enhancer/enabled", enabled);

            if (MessageBridge::instance().hasHandler("dialog.updateConfig")) {
                MessageBridge::instance().invokeHandler("dialog.updateConfig", {{"enabled", enabled}});
            }

            bool restartRequired = false;
            std::string error;
            const bool success = PluginManager::instance().setPluginEnabled(
                normalizedId, enabled, restartRequired, error);
            MessageBridge::instance().pushEvent("dialog:configChanged", {{"enabled", enabled}});
            MessageBridge::instance().pushEvent("plugins:changed", {{"id", normalizedId}, {"enabled", enabled}});
            if (success) {
                releasePluginResourcesAndTrim();
            }
            return {
                {"success", true},
                {"restartRequired", restartRequired},
                {"error", error}
            };
        }

        if (id == "keycast") {
            ConfigManager::instance().set<bool>("/keycast/enabled", enabled);
            ConfigManager::instance().set<bool>("/general/keycastEnabled", enabled);
            ConfigManager::instance().set<bool>("/plugins/keycast/enabled", enabled);

            if (MessageBridge::instance().hasHandler("keycast.updateSettings")) {
                MessageBridge::instance().invokeHandler("keycast.updateSettings", {{"enabled", enabled}});
            }

            bool restartRequired = false;
            std::string error;
            const bool success = PluginManager::instance().setPluginEnabled(
                id, enabled, restartRequired, error);
            MessageBridge::instance().pushEvent("keycast:configChanged", {{"enabled", enabled}});
            MessageBridge::instance().pushEvent("plugins:changed", {{"id", "keycast"}, {"enabled", enabled}});
            if (success) {
                releasePluginResourcesAndTrim();
            }
            return {
                {"success", true},
                {"restartRequired", restartRequired},
                {"error", error}
            };
        }

        if (id == "gesture") {
            ConfigManager::instance().set<bool>("/gesture/enabled", enabled);
            ConfigManager::instance().set<bool>("/gesture/paused", !enabled);
            ConfigManager::instance().set<bool>("/plugins/gesture/enabled", enabled);
            if (MessageBridge::instance().hasHandler("gesture.setPaused")) {
                MessageBridge::instance().invokeHandler("gesture.setPaused", {{"paused", !enabled}});
            }
            bool restartRequired = false;
            std::string error;
            const bool success = PluginManager::instance().setPluginEnabled(
                id, enabled, restartRequired, error);
            MessageBridge::instance().pushEvent("gesture.stateChanged", {{"enabled", enabled}, {"paused", !enabled}});
            MessageBridge::instance().pushEvent("gesture:stateChanged", {{"enabled", enabled}, {"paused", !enabled}});
            MessageBridge::instance().pushEvent("plugins:changed", {{"id", "gesture"}, {"enabled", enabled}});
            if (success) {
                releasePluginResourcesAndTrim();
            }
            return {
                {"success", true},
                {"restartRequired", restartRequired},
                {"error", error}
            };
        }

        if (id == "search") {
            ConfigManager::instance().set<bool>("/search/enabled", enabled);
            ConfigManager::instance().set<bool>("/plugins/search/enabled", enabled);
            bool restartRequired = false;
            std::string error;
            const bool success = PluginManager::instance().setPluginEnabled(
                id, enabled, restartRequired, error);
            MessageBridge::instance().pushEvent("search:stateChanged", {{"enabled", enabled}});
            MessageBridge::instance().pushEvent("plugins:changed", {{"id", "search"}, {"enabled", enabled}});
            if (success) {
                releasePluginResourcesAndTrim();
            }
            return {
                {"success", success},
                {"restartRequired", restartRequired},
                {"error", error}
            };
        }

        if (isExtensionInstalled(id)) {
            return {
                {"success", false},
                {"restartRequired", false},
                {"error", "扩展市场安装器尚未开放，无法启用未加载的扩展包"}
            };
        }

        bool restartRequired = false;
        std::string error;
        const bool success = PluginManager::instance().setPluginEnabled(
            id, enabled, restartRequired, error);
        if (success) {
            MessageBridge::instance().pushEvent("plugins:changed", {{"id", id}, {"enabled", enabled}});
            releasePluginResourcesAndTrim();
        }
        return {
            {"success", success},
            {"restartRequired", restartRequired},
            {"error", error}
        };
    });

    registerHandler("app.trimMemory", [](const json&) -> json {
        releasePluginResourcesAndTrim();
        return {{"success", true}};
    });

    registerHandler("plugins.getMarketplace", [](const json&) -> json {
        const auto& catalog = getMarketplaceCatalog();
        json arr = json::array();
        for (const auto& item : catalog) {
            arr.push_back({
                {"id", item.id},
                {"name", item.name},
                {"nameEn", item.nameEn},
                {"version", item.version},
                {"author", item.author},
                {"description", item.description},
                {"descriptionEn", item.descriptionEn},
                {"category", item.category},
                {"abiVersion", item.abiVersion},
                {"capabilities", item.capabilities},
                {"permissions", item.permissions},
                {"downloadUrl", item.downloadUrl},
                {"installed", isExtensionInstalled(item.id)},
                {"available", false},
                {"featured", item.featured}
            });
        }
        return arr;
    });

    registerHandler("plugins.install", [](const json& params) -> json {
        const std::string id = params.value("id", "");
        if (id.empty()) {
            return {{"success", false}, {"error", "plugin id is required"}};
        }
        return {
            {"success", false},
            {"id", id},
            {"restartRequired", false},
            {"error", "扩展市场当前为预览目录，安全安装与签名校验完成前不会伪装安装成功"}
        };
    });

    registerHandler("plugins.uninstall", [](const json& params) -> json {
        const std::string id = params.value("id", "");
        if (id.empty()) {
            return {{"success", false}, {"error", "plugin id is required"}};
        }
        markExtensionInstalled(id, false);
        return {
            {"success", true},
            {"id", id}
        };
    });

    registerHandler("config.getAll", [](const json&) -> json {
        return json::parse(ConfigManager::instance().toJsonString());
    });
    registerHandler("config.get", [](const json& params) -> json {
        std::string key = params.value("key", "");
        if (key.empty() || key.front() != '/') {
            throw std::invalid_argument("key must be a JSON pointer beginning with '/'");
        }
        auto& config = ConfigManager::instance();
        if (config.has(key)) return config.get<json>(key);
        return nullptr;
    });
    registerHandler("config.set", [](const json& params) -> json {
        std::string key = params.value("key", "");
        if (key.empty() || key.front() != '/') {
            throw std::invalid_argument("key must be a JSON pointer beginning with '/'");
        }
        json value = params.value("value", json{});
        const bool saved = ConfigManager::instance().set(key, value);
        return {{"success", saved}, {"error", saved ? "" : "failed to persist config"}};
    });

    registerHandler("stats.getToday", [](const json&) -> json {
        return StatsManager::instance().getTodayStats().toJson();
    });
    registerHandler("stats.getHistory", [](const json& params) -> json {
        int days = std::clamp(params.value("days", 7), 1, 366);
        return StatsManager::instance().getHistory(days);
    });
    registerHandler("stats.getTotal", [](const json&) -> json {
        return StatsManager::instance().getTotalStats();
    });
    registerHandler("stats.clearToday", [](const json&) -> json {
        StatsManager::instance().clearToday();
        return {{"success", true}};
    });
    registerHandler("stats.getKeyboardLockStates", [](const json&) -> json {
#if defined(_WIN32)
        const bool numLock = (::GetKeyState(VK_NUMLOCK) & 0x0001) != 0;
        const bool capsLock = (::GetKeyState(VK_CAPITAL) & 0x0001) != 0;
        const bool scrollLock = (::GetKeyState(VK_SCROLL) & 0x0001) != 0;
        return {
            {"numLock", numLock},
            {"capsLock", capsLock},
            {"scrollLock", scrollLock}
        };
#else
        return {
            {"numLock", false},
            {"capsLock", false},
            {"scrollLock", false}
        };
#endif
    });

    // ── 性能监控 ─────────────────────────────────────────────────────────
    registerHandler("perf.getMetrics", [](const json&) -> json {
        return PerformanceMonitor::instance().getMetrics().toJson();
    });
    registerHandler("perf.getHistory", [](const json& params) -> json {
        int count = std::clamp(params.value("count", 30), 1, 120);
        auto history = PerformanceMonitor::instance().getHistory(count);
        json arr = json::array();
        for (const auto& m : history) {
            arr.push_back(m.toJson());
        }
        return arr;
    });

    // ── 配置管理（导入/导出/重置）────────────────────────────────────────
    registerHandler("config.export", [](const json& params) -> json {
        std::optional<std::filesystem::path> selected;
        const auto supplied = params.value("path", "");
        if (!supplied.empty()) {
            const std::filesystem::path candidate = WinUtils::utf8ToWstring(supplied);
            if (!isApprovedDirectConfigPath(candidate, false)) {
                LOG_WARN("拒绝 WebView IPC 提供的未授权配置导出路径");
                return {{"success", false}, {"error", "path is outside the approved config directory"}};
            }
            selected = candidate;
        } else {
            selected = choosePath(true);
        }
        if (!selected) return {{"success", false}, {"cancelled", true}};
        auto path = *selected;
        bool ok = ConfigManager::instance().exportTo(path);
        return {{"success", ok}, {"path", WinUtils::wstringToUtf8(path.wstring())}};
    });
    registerHandler("config.import", [](const json& params) -> json {
        std::string path = params.value("path", "");
        if (path.empty()) {
            const auto selected = choosePath(false);
            if (!selected) return {{"success", false}, {"cancelled", true}};
            path = WinUtils::wstringToUtf8(selected->wstring());
        } else {
            const std::filesystem::path candidate = WinUtils::utf8ToWstring(path);
            if (!isApprovedDirectConfigPath(candidate, true)) {
                LOG_WARN("拒绝 WebView IPC 提供的未授权配置导入路径");
                return {{"success", false}, {"error", "path is outside the approved config directory"}};
            }
        }
        bool ok = ConfigManager::instance().importFrom(WinUtils::utf8ToWstring(path));
        return {{"success", ok}};
    });
    registerHandler("config.reset", [](const json&) -> json {
        // 只有备份成功后才允许清空，避免磁盘异常时造成不可逆的数据丢失。
        if (!ConfigManager::instance().exportTo(
                WinUtils::getConfigDirectory() / "config_backup.json")) {
            return {{"success", false}, {"error", "failed to back up config"}};
        }
        const bool reset = ConfigManager::instance().reset();
        if (reset) LOG_INFO("配置已重置为默认值");
        return {{"success", reset}, {"error", reset ? "" : "failed to persist config"}};
    });

    // ── 快捷键管理 ───────────────────────────────────────────────────────
    registerHandler("hotkey.getAll", [](const json&) -> json {
        json hotkeys = json::array();
        for (const auto& entry : HotkeyManager::instance().getAllHotkeys()) {
            hotkeys.push_back({
                {"name", entry.name},
                {"shortcut", entry.def.toString()},
                {"registered", entry.registered},
                {"armed", entry.armed},
                {"conflict", entry.conflict},
                {"conflictType", entry.conflictType},
                {"conflictWith", entry.conflictWith}
            });
        }
        std::sort(hotkeys.begin(), hotkeys.end(), [](const json& a, const json& b) {
            return a.value("name", "") < b.value("name", "");
        });
        return hotkeys;
    });
    registerHandler("hotkey.check", [](const json& params) -> json {
        const std::string name = canonicalHotkeyName(params.value("name", ""));
        const std::string text = params.value("hotkey", "");
        if (text.empty()) {
            return {{"conflict", false}, {"conflictType", "none"}, {"conflictWith", ""}};
        }
        const auto parsed = HotkeyDef::fromString(text);
        if (!parsed) {
            return {{"conflict", true}, {"conflictType", "invalid"}, {"conflictWith", "无效的快捷键格式"}};
        }
        auto info = HotkeyManager::instance().checkConflict(*parsed, name);
        return {
            {"conflict", info.hasConflict},
            {"conflictType", info.conflictType},
            {"conflictWith", info.conflictWith}
        };
    });
    registerHandler("hotkey.rebind", [](const json& params) -> json {
        const std::string name = canonicalHotkeyName(params.value("name", ""));
        const std::string text = params.value("hotkey", "");
        if (name.empty()) {
            return {{"success", false}, {"error", "invalid hotkey"}};
        }
        auto& manager = HotkeyManager::instance();
        const auto entries = manager.getAllHotkeys();
        const auto previous = std::ranges::find_if(entries, [&name](const auto& entry) {
            return entry.name == name;
        });
        if (previous == entries.end()) {
            return {{"success", false}, {"error", "unknown hotkey"}};
        }
        if (text.empty()) {
            if (!manager.clearHotkey(name)) {
                return {{"success", false}, {"error", "could not disable hotkey"}};
            }
            if (!ConfigManager::instance().set("/hotkeys/" + name, "")) {
                if (previous->def.virtualKey != 0) manager.rebindHotkey(name, previous->def);
                return {{"success", false}, {"error", "failed to persist hotkey"},
                        {"name", name}, {"shortcut", previous->def.toString()}};
            }
            return {{"success", true}, {"name", name}, {"shortcut", ""}};
        }
        const auto parsed = HotkeyDef::fromString(text);
        if (!parsed) return {{"success", false}, {"error", "invalid hotkey"}};
        if (!manager.rebindHotkey(name, *parsed)) {
            auto conflictInfo = manager.checkConflict(*parsed, name);
            std::string errMsg = conflictInfo.hasConflict && !conflictInfo.conflictWith.empty()
                ? conflictInfo.conflictWith
                : "快捷键已被其他程序或系统占用";
            return {{"success", false}, {"error", errMsg},
                    {"conflictType", conflictInfo.conflictType},
                    {"conflictWith", conflictInfo.conflictWith},
                    {"name", name}, {"shortcut", previous->def.toString()}};
        }
        if (!ConfigManager::instance().set("/hotkeys/" + name, parsed->toString())) {
            const bool rolledBack = previous->def.virtualKey == 0
                ? manager.clearHotkey(name)
                : manager.rebindHotkey(name, previous->def);
            if (!rolledBack) {
                LOG_ERROR("快捷键持久化失败且运行时回滚失败: name={}", name);
            }
            return {{"success", false}, {"error", "failed to persist hotkey"},
                    {"name", name}, {"shortcut", previous->def.toString()}};
        }
        return {{"success", true}, {"name", name}, {"shortcut", parsed->toString()}};
    });
    registerHandler("hotkey.setPaused", [](const json& params) -> json {
        const bool paused = params.value("paused", false);
        HotkeyManager::instance().setPaused(paused);
        return {{"success", true}, {"paused", paused}};
    });

    // ── 通用设置与系统交互 ───────────────────────────────────────────────
    registerHandler("general.getSettings", [](const json&) -> json {
        auto& config = ConfigManager::instance();
        bool autoStart = false;
        if (config.has("/general/autoStart")) {
            const bool desiredAutoStart = config.get<bool>("/general/autoStart", false);
            if (desiredAutoStart) {
                autoStart = getCachedAutoStart();
                if (!autoStart) {
                    LOG_WARN("AutoStart is enabled in settings but its OS registration is missing; recreating it.");
                    autoStart = setAutoStart(true);
                    if (autoStart) LOG_INFO("Missing AutoStart registration recreated successfully.");
                    else LOG_ERROR("Failed to recreate missing AutoStart registration.");
                }
            }
        } else {
            // Migrate installations that predate the persisted /general/autoStart key.
            autoStart = getCachedAutoStart();
        }
        return {
            {"autoStart", autoStart},
            {"runAsAdmin", config.get<bool>("/general/runAsAdmin", true)},
            {"elevated", WinUtils::isCurrentProcessElevated()},
            {"minimizeToTray", config.get<bool>("/general/minimizeToTray", true)},
            {"checkUpdates", config.get<bool>("/general/checkUpdates", true)},
            {"autoReleaseSettingsMemory", config.get<bool>("/general/autoReleaseSettingsMemory", true)},
            {"keycastEnabled", config.get<bool>("/general/keycastEnabled", true)},
            {"showOnboarding", config.get<bool>("/general/showOnboarding", false)},
            {"isPortableMode", WinUtils::isPortableMode()},
            {"dataDirectory", WinUtils::wstringToUtf8(WinUtils::getAppDataDirectory().wstring())},
            {"language", config.get<std::string>("/general/language", "auto")},
            {"fontFamily", config.get<std::string>("/general/fontFamily", "auto")},
            {"logLevel", config.get<std::string>("/general/logLevel", "info")},
            {"logRetentionDays", config.get<uint32_t>("/general/logRetentionDays", 7)},
            {"theme", config.get<std::string>("/general/theme", "system")},
            {"accentColor", config.get<std::string>("/general/accentColor", "blue")},
            {"trayIconTheme", config.get<std::string>("/general/trayIconTheme", "system")},
        };
    });
    registerHandler("autoStart.set", [](const json& params) -> json {
        bool enabled = params.value("enabled", params.value("autoStart", false));
        bool success = setAutoStart(enabled);
        if (success) {
            ConfigManager::instance().set("/general/autoStart", enabled);
        }
        return {{"success", success}, {"enabled", enabled}};
    });
    registerHandler("autoStart.get", [](const json&) -> json {
        return {{"enabled", getCachedAutoStart()}};
    });
    registerHandler("general.updateSettings", [](const json& params) -> json {
        static const std::unordered_set<std::string> boolKeys = {
            "autoStart", "runAsAdmin", "minimizeToTray", "checkUpdates", "autoReleaseSettingsMemory", "keycastEnabled", "showOnboarding"
        };
        static const std::unordered_set<std::string> themes = {"system", "light", "dark"};
        static const std::unordered_set<std::string> trayIconThemes = {"system", "auto", "light", "dark", "color"};
        static const std::unordered_set<std::string> logLevels = {"trace", "debug", "info", "warn", "error"};
        static const std::unordered_set<std::string> fontFamilies = {
            "auto", "noto-sans-sc", "harmony-sans", "yahei", "pingfang", "system"
        };
        static const std::unordered_set<std::string> readOnlyKeys = {
            "elevated", "isPortableMode", "dataDirectory"
        };
        static const std::unordered_set<std::string> accents = {
            "violet", "cyan", "amber", "blue", "mint", "coral"
        };
        static const std::unordered_set<std::string> languages = {
            "auto", "zh-CN", "en-US", "zh-TW", "zh-HK", "ja-JP", "ko-KR", "de-DE", "fr-FR", "es-ES"
        };
        if (!params.is_object() || params.empty()) {
            return {{"success", false}, {"error", "no settings supplied"}};
        }
        auto& config = ConfigManager::instance();
        const bool previousAutoStart = config.get<bool>("/general/autoStart", false);
        for (const auto& [key, value] : params.items()) {
            if (readOnlyKeys.contains(key)) {
                continue;
            }
            if (boolKeys.contains(key) && !value.is_boolean()) {
                return {{"success", false}, {"error", key + " must be boolean"}};
            }
            if (key == "theme" && (!value.is_string() || !themes.contains(value.get<std::string>()))) {
                return {{"success", false}, {"error", "invalid theme"}};
            }
            if (key == "trayIconTheme" && (!value.is_string() || !trayIconThemes.contains(value.get<std::string>()))) {
                return {{"success", false}, {"error", "invalid tray icon theme"}};
            }
            if (key == "accentColor" && (!value.is_string() || !accents.contains(value.get<std::string>()))) {
                return {{"success", false}, {"error", "invalid accent color"}};
            }
            if (key == "logLevel" && (!value.is_string() || !logLevels.contains(value.get<std::string>()))) {
                return {{"success", false}, {"error", "invalid log level"}};
            }
            if (key == "language" && (!value.is_string() ||
                !languages.contains(value.get<std::string>()))) {
                return {{"success", false}, {"error", "invalid language"}};
            }
            if (key == "fontFamily" && (!value.is_string() ||
                !fontFamilies.contains(value.get<std::string>()))) {
                return {{"success", false}, {"error", "invalid font family"}};
            }
            if (key == "logRetentionDays") {
                if (!value.is_number_integer() || value.get<int64_t>() < 0 || value.get<int64_t>() > 3650) {
                    return {{"success", false}, {"error", "invalid log retention days"}};
                }
            }
            if (!boolKeys.contains(key) && key != "theme" && key != "accentColor" && key != "logLevel" && key != "language" && key != "fontFamily" && key != "trayIconTheme" && key != "logRetentionDays") {
                return {{"success", false}, {"error", "unsupported setting: " + key}};
            }
        }
        if (params.contains("autoStart") && !setAutoStart(params["autoStart"].get<bool>())) {
            return {{"success", false}, {"error", "failed to update auto-start"}};
        }
        if (params.contains("showOnboarding")) {
            const bool showOb = params["showOnboarding"].get<bool>();
            config.set<bool>("/app/onboardingCompleted", !showOb);
        }
        const bool saved = config.mergePatch({{"general", params}}, "/general");
        if (!saved) {
            if (params.contains("autoStart")) setAutoStart(previousAutoStart);
            return {{"success", false}, {"error", "failed to persist settings"}};
        }
        if (params.contains("language")) {
            Logger::setLanguage(params["language"].get<std::string>());
        }
        if (params.contains("logLevel")) applyLogLevel(params["logLevel"].get<std::string>());
        if (params.contains("logRetentionDays")) {
            Logger::setRetentionDays(params["logRetentionDays"].get<uint32_t>());
        }
        if (params.contains("theme") || params.contains("accentColor") || params.contains("trayIconTheme")) {
            EventBus::instance().publish(ThemeChangedEvent{
                config.get<std::string>("/general/theme", "system"),
                config.get<std::string>("/general/accentColor", "blue"),
                config.get<std::string>("/general/trayIconTheme", "system")
            });
        }
        return {{"success", true}};
    });
    registerHandler("capture.browseDirectory", [](const json&) -> json {
        const auto selected = choosePath(false, true);
        return selected ? json(WinUtils::wstringToUtf8(selected->wstring())) : json(nullptr);
    });
    registerSystemInteractionHandlers(*this);
    registerHandler("app.checkForUpdates", [](const json&) -> json {
        const bool started = UpdateChecker::instance().checkAsync(true);
        return {{"success", true}, {"started", started}};
    });
    // ── 应用系统信息 ─────────────────────────────────────────────────────
    registerHandler("app.getSystemInfo", [](const json&) -> json {
        SYSTEM_INFO si;
        GetSystemInfo(&si);
        
        MEMORYSTATUSEX memInfo;
        memInfo.dwLength = sizeof(memInfo);
        GlobalMemoryStatusEx(&memInfo);

        std::string arch = "x64";
        if (si.wProcessorArchitecture == PROCESSOR_ARCHITECTURE_ARM64) {
            arch = "ARM64";
        } else if (si.wProcessorArchitecture == PROCESSOR_ARCHITECTURE_INTEL) {
            arch = "x86";
        }

        OSVERSIONINFOEXW osvi{};
        osvi.dwOSVersionInfoSize = sizeof(osvi);

        return {
            {"version", UpdateChecker::CurrentVersion},
            {"cpuArch", arch},
            {"cpuCores", si.dwNumberOfProcessors},
            {"totalMemoryGB", memInfo.ullTotalPhys / (1024.0 * 1024.0 * 1024.0)},
            {"dpiScale", WinUtils::getDpiScale()},
            {"language", WinUtils::isSystemLanguageChinese() ? "zh-CN" : "en-US"}
        };
    });

    // ── 打开的窗口与进程枚举 ─────────────────────────────────────────────
    registerHandler("window.getOpenWindows", [](const json&) -> json {
        struct WindowItem {
            std::string title;
            std::string processName;
            std::string windowClass;
            DWORD pid = 0;
        };

        std::vector<WindowItem> windows;
        EnumWindows([](HWND hwnd, LPARAM lParam) -> BOOL {
            if (!IsWindowVisible(hwnd)) return TRUE;
            if (GetWindowTextLengthW(hwnd) == 0) return TRUE;

            RECT rc{};
            if (GetWindowRect(hwnd, &rc)) {
                if ((rc.right - rc.left) <= 50 || (rc.bottom - rc.top) <= 50) return TRUE;
            }

            LONG exStyle = GetWindowLongW(hwnd, GWL_EXSTYLE);
            if (exStyle & WS_EX_TOOLWINDOW) return TRUE;

            wchar_t cls[256]{};
            GetClassNameW(hwnd, cls, 256);
            std::wstring clsStr = cls;
            if (clsStr == L"Progman" || clsStr == L"Shell_TrayWnd" || 
                clsStr == L"Windows.UI.Core.CoreWindow") {
                return TRUE;
            }

            DWORD pid = 0;
            GetWindowThreadProcessId(hwnd, &pid);
            if (pid == 0 || pid == GetCurrentProcessId()) return TRUE;

            std::wstring procName = WinUtils::processNameFromPid(pid);
            if (procName.empty()) return TRUE;

            std::wstring title = WinUtils::getWindowTitle(hwnd);
            if (title.empty()) return TRUE;

            auto* list = reinterpret_cast<std::vector<WindowItem>*>(lParam);
            std::string u8Proc = WinUtils::wstringToUtf8(procName);
            std::string u8Title = WinUtils::wstringToUtf8(title);
            std::string u8Class = WinUtils::wstringToUtf8(clsStr);

            for (const auto& item : *list) {
                if (item.processName == u8Proc && item.title == u8Title) {
                    return TRUE;
                }
            }

            list->push_back({ u8Title, u8Proc, u8Class, pid });
            return TRUE;
        }, reinterpret_cast<LPARAM>(&windows));

        json arr = json::array();
        for (const auto& w : windows) {
            arr.push_back({
                {"title", w.title},
                {"processName", w.processName},
                {"windowClass", w.windowClass},
                {"pid", w.pid}
            });
        }
        return {{"windows", std::move(arr)}};
    });

    registerHandler("window.getForegroundInfo", [](const json&) -> json {
        HWND hwnd = GetForegroundWindow();
        if (!hwnd) return {{"success", false}};
        DWORD pid = 0;
        GetWindowThreadProcessId(hwnd, &pid);
        std::wstring procName = WinUtils::processNameFromPid(pid);
        std::wstring title = WinUtils::getWindowTitle(hwnd);
        std::wstring cls = WinUtils::getWindowClassName(hwnd);
        return {
            {"success", true},
            {"title", WinUtils::wstringToUtf8(title)},
            {"processName", WinUtils::wstringToUtf8(procName)},
            {"windowClass", WinUtils::wstringToUtf8(cls)},
            {"pid", pid}
        };
    });

    LOG_INFO("内置核心 IPC 处理器注册完成（含性能监控、配置管理、系统信息、窗口枚举）");
}

}  // namespace tools3000::core
