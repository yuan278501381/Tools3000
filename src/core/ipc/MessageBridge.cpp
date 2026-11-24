// ─────────────────────────────────────────────────────────────────────────────
// MessageBridge.cpp — WebView2 ↔ C++ IPC 桥接层实现
// ─────────────────────────────────────────────────────────────────────────────

#include "core/ipc/MessageBridge.h"
#include "core/i18n/UiI18n.h"
#include "core/ipc/SystemInteractionHandlers.h"
#include "core/ipc/AutoStartPolicy.h"
#include "core/logger/Logger.h"
#include "core/plugin/PluginManager.h"
#include "core/utils/TraceId.h"
#include "core/config/ConfigManager.h"
#include "core/hotkey/HotkeyManager.h"
#include "core/hotkey/MouseHook.h"
#include "core/hotkey/KeyboardHook.h"
#include "core/utils/ThemeUtils.h"
#include "core/utils/WinUtils.h"
#include "core/utils/OutOfProcessFileDialog.h"
#include "core/utils/FileLocksmithEngine.h"
#include "core/stats/StatsManager.h"
#include "core/stats/PerformanceMonitor.h"
#include "core/update/UpdateChecker.h"
#include "core/backup/ConfigBackupService.h"
#include "core/events/EventBus.h"
#include "core/events/MainThreadDispatcher.h"
#include "core/remote/RemoteMasterEngine.h"
#include "core/utils/UiThreadJoin.h"
#include "Tools3000Version.h"
#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cstddef>
#include <cwchar>
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
constexpr size_t MaxFeedbackBridgeMessageBytes = 12 * 1024 * 1024;

size_t resolveBridgeMessageSizeLimit(std::string_view messageJson) noexcept {
    const std::string_view head = messageJson.substr(0, (messageJson.size() < 512u) ? messageJson.size() : 512u);
    const size_t keyPos = head.find("\"method\"");
    if (keyPos != std::string_view::npos) {
        size_t pos = keyPos + 8;
        while (pos < head.size() && (head[pos] == ' ' || head[pos] == '\t' || head[pos] == '\r' || head[pos] == '\n')) ++pos;
        if (pos < head.size() && head[pos] == ':') {
            ++pos;
            while (pos < head.size() && (head[pos] == ' ' || head[pos] == '\t' || head[pos] == '\r' || head[pos] == '\n')) ++pos;
            if (pos < head.size() && head[pos] == '"') {
                ++pos;
                const size_t endQuote = head.find('"', pos);
                if (endQuote != std::string_view::npos) {
                    const std::string_view method = head.substr(pos, endQuote - pos);
                    if (method == "feedback.submit" || method == "feedback.preview") {
                        return MaxFeedbackBridgeMessageBytes;
                    }
                }
            }
        }
    }
    return MaxBridgeMessageBytes;
}

int extractFallbackRequestId(std::string_view messageJson) noexcept {
    const std::string_view head = messageJson.substr(0, (messageJson.size() < 512u) ? messageJson.size() : 512u);
    const size_t keyPos = head.find("\"id\"");
    if (keyPos == std::string_view::npos) return 0;
    size_t pos = keyPos + 4;
    while (pos < head.size() && (head[pos] == ' ' || head[pos] == '\t' || head[pos] == '\r' || head[pos] == '\n')) ++pos;
    if (pos >= head.size() || head[pos] != ':') return 0;
    ++pos;
    while (pos < head.size() && (head[pos] == ' ' || head[pos] == '\t' || head[pos] == '\r' || head[pos] == '\n')) ++pos;
    long long val = 0;
    bool hasDigit = false;
    while (pos < head.size() && head[pos] >= '0' && head[pos] <= '9') {
        hasDigit = true;
        val = val * 10 + (head[pos] - '0');
        if (val > 2147483647LL) return 0;
        ++pos;
    }
    return hasDigit ? static_cast<int>(val) : 0;
}

std::string bridgeErrorResponse(int id, int code, std::string_view message) {
    return json{{"id", id}, {"error", {{"code", code}, {"message", std::string(message)}}}}
        .dump(-1, ' ', false, json::error_handler_t::replace);
}

std::string bridgeRawSuccessResponse(int id, std::string_view rawResultJson) {
    std::string trimmed = std::string(rawResultJson);
    while (!trimmed.empty() && static_cast<unsigned char>(trimmed.front()) <= 0x20) {
        trimmed.erase(trimmed.begin());
    }
    while (!trimmed.empty() && static_cast<unsigned char>(trimmed.back()) <= 0x20) {
        trimmed.pop_back();
    }
    if (trimmed.empty()) {
        trimmed = "null";
    }
    std::string out;
    out.reserve(24 + trimmed.size());
    out.append("{\"id\":");
    out.append(std::to_string(id));
    out.append(",\"result\":");
    out.append(trimmed);
    out.push_back('}');
    return out;
}

// 零堆分配顶层 JSON-RPC 头部扫描器：在 UI 主线程极速提取顶层整数 "id" 与无转义字符串 "method"，
// 彻底消除异步 IPC 在主线程与工作线程的双重全量 json::parse 开销；任何非常规/转义/畸形文本均安全回退慢路径。
bool tryFastExtractJsonRpcHeader(std::string_view text, int& outId, std::string& outMethod) noexcept {
    auto skipWs = [&](size_t& pos) noexcept {
        while (pos < text.size()) {
            const char c = text[pos];
            if (c != ' ' && c != '\t' && c != '\r' && c != '\n') break;
            ++pos;
        }
    };

    auto skipString = [&](size_t& pos, std::string_view* plainSlice, bool& hadEscape) noexcept -> bool {
        if (pos >= text.size() || text[pos] != '"') return false;
        ++pos;
        const size_t start = pos;
        hadEscape = false;
        while (pos < text.size()) {
            const unsigned char c = static_cast<unsigned char>(text[pos]);
            if (c < 0x20) return false; // JSON 字符串内禁止裸控制字符
            if (c == '\\') {
                hadEscape = true;
                ++pos;
                if (pos >= text.size()) return false;
                const char esc = text[pos];
                if (esc == 'u') {
                    if (pos + 4 >= text.size()) return false;
                    for (size_t k = 1; k <= 4; ++k) {
                        const char hc = text[pos + k];
                        const bool isHex = (hc >= '0' && hc <= '9') ||
                                           (hc >= 'a' && hc <= 'f') ||
                                           (hc >= 'A' && hc <= 'F');
                        if (!isHex) return false;
                    }
                    pos += 5;
                    continue;
                }
                if (esc != '"' && esc != '\\' && esc != '/' && esc != 'b' &&
                    esc != 'f' && esc != 'n' && esc != 'r' && esc != 't') {
                    return false;
                }
                ++pos;
                continue;
            }
            if (c == '"') {
                if (plainSlice) *plainSlice = text.substr(start, pos - start);
                ++pos;
                return true;
            }
            ++pos;
        }
        return false;
    };

    size_t pos = 0;
    skipWs(pos);
    if (pos >= text.size() || text[pos] != '{') return false;
    ++pos;

    int parsedId = 0;
    bool foundMethod = false;
    std::string_view methodView;
    int depth = 1;

    // 顶层键值对循环（depth == 1）
    for (;;) {
        skipWs(pos);
        if (pos >= text.size()) return false;
        if (text[pos] == '}') {
            ++pos;
            --depth;
            break;
        }
        std::string_view keyView;
        bool keyEsc = false;
        if (!skipString(pos, &keyView, keyEsc)) return false;
        skipWs(pos);
        if (pos >= text.size() || text[pos] != ':') return false;
        ++pos;
        skipWs(pos);
        if (pos >= text.size()) return false;

        if (!keyEsc && keyView == "id") {
            bool neg = false;
            if (text[pos] == '-') {
                neg = true;
                ++pos;
            }
            if (pos >= text.size() || text[pos] < '0' || text[pos] > '9') return false;
            long long val = 0;
            while (pos < text.size() && text[pos] >= '0' && text[pos] <= '9') {
                val = val * 10 + (text[pos] - '0');
                if (val > 2147483647LL) return false;
                ++pos;
            }
            // 若紧跟小数点或指数，交给完整解析器处理
            if (pos < text.size() && (text[pos] == '.' || text[pos] == 'e' || text[pos] == 'E')) {
                return false;
            }
            parsedId = static_cast<int>(neg ? -val : val);
        } else if (!keyEsc && keyView == "method") {
            bool valEsc = false;
            std::string_view mSlice;
            if (!skipString(pos, &mSlice, valEsc) || valEsc) return false;
            methodView = mSlice;
            foundMethod = true;
        } else {
            // 跳过任意合法 JSON value（嵌套对象/数组/字符串/字面量/数字）
            const char vc = text[pos];
            if (vc == '"') {
                bool dummyEsc = false;
                if (!skipString(pos, nullptr, dummyEsc)) return false;
            } else if (vc == '{' || vc == '[') {
                std::vector<char> stack;
                stack.reserve(8);
                stack.push_back(vc == '{' ? '}' : ']');
                ++pos;
                while (!stack.empty()) {
                    if (pos >= text.size()) return false;
                    const char nc = text[pos];
                    if (nc == '"') {
                        bool dummyEsc = false;
                        if (!skipString(pos, nullptr, dummyEsc)) return false;
                        continue;
                    }
                    if (nc == '{') {
                        stack.push_back('}');
                    } else if (nc == '[') {
                        stack.push_back(']');
                    } else if (nc == '}' || nc == ']') {
                        if (stack.back() != nc) return false;
                        stack.pop_back();
                    }
                    ++pos;
                }
            } else {
                // 数字、true、false、null
                const size_t litStart = pos;
                while (pos < text.size()) {
                    const char lc = text[pos];
                    if (lc == ',' || lc == '}' || lc == ' ' || lc == '\t' || lc == '\r' || lc == '\n') break;
                    if (lc == '"' || lc == '{' || lc == '[' || lc == ']') return false;
                    ++pos;
                }
                if (pos == litStart) return false;
            }
        }

        skipWs(pos);
        if (pos >= text.size()) return false;
        if (text[pos] == ',') {
            ++pos;
            continue;
        }
        if (text[pos] == '}') {
            ++pos;
            --depth;
            break;
        }
        return false;
    }

    skipWs(pos);
    if (depth != 0 || pos != text.size() || !foundMethod) return false;
    outId = parsedId;
    outMethod.assign(methodView.data(), methodView.size());
    return true;
}

std::optional<std::filesystem::path> choosePath(bool save, bool folder = false) {
    const bool isZh = tools3000::core::isUiLanguageChinese();
    tools3000::core::FileDialogRequest req{};
    req.ownerHwnd = nullptr;
    if (folder) {
        req.mode = tools3000::core::FileDialogMode::PickFolder;
        req.title = tools3000::core::trWCstrFor(isZh, L"选择保存目录", L"Select Save Directory");
    } else {
        req.mode = save ? tools3000::core::FileDialogMode::SaveFile : tools3000::core::FileDialogMode::OpenFile;
        req.filters = {
            {tools3000::core::trWCstrFor(isZh, L"Tools3000 配置 (*.json)", L"Tools3000 Configuration (*.json)"), L"*.json"},
            {tools3000::core::trWCstrFor(isZh, L"所有文件 (*.*)", L"All Files (*.*)"), L"*.*"},
        };
        req.defaultExtension = L"json";
        if (save) {
            req.defaultFileName = L"Tools3000-config.json";
        }
        req.title = save
            ? tools3000::core::trWCstrFor(isZh, L"导出 Tools3000 配置", L"Export Tools3000 Configuration")
            : tools3000::core::trWCstrFor(isZh, L"导入 Tools3000 配置", L"Import Tools3000 Configuration");
    }
    const auto res = tools3000::core::OutOfProcessFileDialog::show(req);
    if (!res.accepted || res.selectedPath.empty()) {
        return std::nullopt;
    }
    return res.selectedPath;
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

std::wstring decodeCapturedProcessText(const std::string& raw) {
    if (raw.empty()) return {};
    if (raw.size() >= 2 &&
        static_cast<unsigned char>(raw[0]) == 0xFF &&
        static_cast<unsigned char>(raw[1]) == 0xFE) {
        const size_t wcharCount = (raw.size() - 2) / sizeof(wchar_t);
        std::wstring wide(wcharCount, L'\0');
        if (wcharCount > 0) {
            std::memcpy(wide.data(), raw.data() + 2, wcharCount * sizeof(wchar_t));
        }
        while (!wide.empty() && wide.back() == L'\0') {
            wide.pop_back();
        }
        return wide;
    }
    std::string_view slice(raw);
    if (slice.size() >= 3 &&
        static_cast<unsigned char>(slice[0]) == 0xEF &&
        static_cast<unsigned char>(slice[1]) == 0xBB &&
        static_cast<unsigned char>(slice[2]) == 0xBF) {
        slice.remove_prefix(3);
    }
    if (slice.empty()) return {};
    const int utf8Len = MultiByteToWideChar(
        CP_UTF8, MB_ERR_INVALID_CHARS, slice.data(), static_cast<int>(slice.size()), nullptr, 0);
    if (utf8Len > 0) {
        std::wstring wide(static_cast<size_t>(utf8Len), L'\0');
        MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, slice.data(),
                            static_cast<int>(slice.size()), wide.data(), utf8Len);
        return wide;
    }
    const int acpLen = MultiByteToWideChar(
        CP_ACP, 0, slice.data(), static_cast<int>(slice.size()), nullptr, 0);
    if (acpLen > 0) {
        std::wstring wide(static_cast<size_t>(acpLen), L'\0');
        MultiByteToWideChar(CP_ACP, 0, slice.data(),
                            static_cast<int>(slice.size()), wide.data(), acpLen);
        return wide;
    }
    return {};
}

std::optional<std::wstring> queryAutoStartTaskXml() {
    std::string output;
    // 优先查询当前用户隔离的任务计划 (Tools3000\Autorun for <User>)
    std::wstring taskName = tools3000::core::autostart::getAutoStartTaskName();
    std::wstring queryCmd = L"schtasks.exe /query /tn \"" + taskName + L"\" /xml";
    if (executeSilentCommand(queryCmd, &output) && !output.empty()) {
        std::wstring decoded = decodeCapturedProcessText(output);
        if (!decoded.empty()) {
            return decoded;
        }
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

    // 4. 查询计划任务 XML 内容校验（纯只读探测，绝不在只读状态查询中暗中触发 UAC 写操作）
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
            return false;
        }
        LOG_WARN("AutoStart task points to a different executable path; reporting disabled for current binary.");
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
        bool taskCleared = true;
        const bool hasScheduledTask =
            tools3000::core::autostart::isTaskRegisteredCOM(exeStr) ||
            queryAutoStartTaskXml().has_value();
        if (hasScheduledTask) {
            if (tools3000::core::WinUtils::isCurrentProcessElevated()) {
                taskCleared = tools3000::core::autostart::unregisterTaskCOM();
            } else if (!tools3000::core::autostart::unregisterTaskCOM()) {
                taskCleared = false;
                SHELLEXECUTEINFOW sei = {sizeof(sei)};
                sei.fMask = SEE_MASK_NOCLOSEPROCESS;
                sei.lpVerb = L"runas";
                sei.lpFile = exeStr.c_str();
                sei.lpParameters = L"--unregister-autostart";
                sei.lpDirectory = exeDir.c_str();
                sei.nShow = SW_HIDE;
                if (ShellExecuteExW(&sei) && sei.hProcess) {
                    if (WaitForSingleObject(sei.hProcess, 5000) == WAIT_OBJECT_0) {
                        DWORD exitCode = 1;
                        if (GetExitCodeProcess(sei.hProcess, &exitCode) && exitCode == 0) {
                            taskCleared = true;
                        }
                    }
                    CloseHandle(sei.hProcess);
                }
            }
        }
        removeRegistryAutoStart();
        if (!taskCleared) {
            LOG_WARN("AutoStart Task Scheduler entry could not be removed (elevation denied or failed).");
            return false;
        }
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

void syncRuntimeSettingsFromConfig(const char* source) {
    auto& config = tools3000::core::ConfigManager::instance();
    const bool desiredAutoStart = config.get<bool>("/general/autoStart", false);
    setAutoStart(desiredAutoStart);

    const std::string language = config.get<std::string>("/general/language", "auto");
    tools3000::core::UiI18n::setConfiguredLanguage(language);
    tools3000::core::Logger::setLanguage(language);
    applyLogLevel(config.get<std::string>("/general/logLevel", "info"));
    tools3000::core::Logger::setRetentionDays(config.get<uint32_t>("/general/logRetentionDays", 7));

    tools3000::core::KeyboardHook::instance().syncConfigCache();

    auto& hotkeyMgr = tools3000::core::HotkeyManager::instance();
    for (const auto& entry : hotkeyMgr.getAllHotkeys()) {
        const std::string cfgKey = "/hotkeys/" + entry.name;
        if (!config.has(cfgKey)) {
            continue;
        }
        const std::string text = config.get<std::string>(cfgKey, "");
        if (text.empty()) {
            hotkeyMgr.clearHotkey(entry.name);
        } else if (const auto parsed = tools3000::core::HotkeyDef::fromString(text)) {
            hotkeyMgr.rebindHotkey(entry.name, *parsed);
        }
    }

    const std::string theme = config.get<std::string>("/general/theme", "system");
    const std::string accentColor = config.get<std::string>("/general/accentColor", "blue");
    const std::string trayIconTheme = config.get<std::string>("/general/trayIconTheme", "system");
    const std::string fontFamily = config.get<std::string>("/general/fontFamily", "auto");

    tools3000::core::WinUtils::applyNativeMenuTheme(theme);
    tools3000::core::EventBus::instance().publish(tools3000::core::ThemeChangedEvent{
        theme,
        accentColor,
        trayIconTheme
    });
    tools3000::core::MessageBridge::instance().pushEvent("appearance:changed", {
        {"theme", theme},
        {"accentColor", accentColor},
        {"trayIconTheme", trayIconTheme},
        {"language", language},
        {"fontFamily", fontFamily}
    });
    tools3000::core::MessageBridge::instance().pushEvent("config:changed", {
        {"source", source}
    });
}

std::atomic<bool> g_hasQuarantinedHandlers{false};

}  // namespace

namespace tools3000::core {

MessageBridge& MessageBridge::instance() {
    static MessageBridge inst;
    return inst;
}

void MessageBridge::registerHandlerImpl(const std::string& method,
                                        MessageHandler handler,
                                        bool isHostOwned) {
    auto slot = std::make_shared<HandlerSlot>();
    slot->handler = std::move(handler);
    slot->isHostOwned = isHostOwned;
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

void MessageBridge::registerHandler(const std::string& method, MessageHandler handler) {
    registerHandlerImpl(method, std::move(handler), false);
}

void MessageBridge::registerAsyncHandler(const std::string& method, MessageHandler handler) {
    markMethodAsync(method);
    registerHandlerImpl(method, std::move(handler), false);
}

void MessageBridge::registerHostHandler(const std::string& method, MessageHandler handler) {
    registerHandlerImpl(method, std::move(handler), true);
}

void MessageBridge::registerAsyncHostHandler(const std::string& method, MessageHandler handler) {
    markMethodAsync(method);
    registerHandlerImpl(method, std::move(handler), true);
}

void MessageBridge::registerRawHandlerImpl(const std::string& method,
                                           RawMessageHandler handler,
                                           bool isHostOwned) {
    auto slot = std::make_shared<HandlerSlot>();
    slot->rawHandler = std::move(handler);
    slot->isHostOwned = isHostOwned;
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

void MessageBridge::registerRawHandler(const std::string& method, RawMessageHandler handler) {
    registerRawHandlerImpl(method, std::move(handler), false);
}

void MessageBridge::registerAsyncRawHandler(const std::string& method, RawMessageHandler handler) {
    markMethodAsync(method);
    registerRawHandlerImpl(method, std::move(handler), false);
}

void MessageBridge::registerRawHostHandler(const std::string& method, RawMessageHandler handler) {
    registerRawHandlerImpl(method, std::move(handler), true);
}

void MessageBridge::registerAsyncRawHostHandler(const std::string& method, RawMessageHandler handler) {
    markMethodAsync(method);
    registerRawHandlerImpl(method, std::move(handler), true);
}

bool MessageBridge::hasQuarantinedHandlers() const noexcept {
    return g_hasQuarantinedHandlers.load(std::memory_order_acquire);
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
            g_hasQuarantinedHandlers.store(true, std::memory_order_release);
            lock.unlock();
            std::lock_guard quarantineLock(*quarantinedMutex);
            quarantinedSlots->push_back(slot);
            continue;
        }
        if (!boundedForShutdown) {
            slot->idle.wait(lock, [&slot]() { return slot->activeCalls == 0; });
        }
        slot->handler = nullptr;
        slot->rawHandler = nullptr;
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
            if (it->first.starts_with(prefix) && !it->second->isHostOwned) {
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
    const size_t maxBytes = resolveBridgeMessageSizeLimit(messageJson);
    if (messageJson.size() > maxBytes) {
        id = extractFallbackRequestId(messageJson);
        LOG_WARN("拒绝过大的 IPC 消息: {} bytes", messageJson.size());
        return bridgeErrorResponse(id, -32600, "Request exceeds size limit");
    }
    try {
        auto request = json::parse(messageJson);
        id = request.value("id", 0);
        std::string method = request.value("method", "");
        json params = (request.contains("params") && !request["params"].is_null()) ? request["params"] : json::object();

        LOG_DEBUG("收到前端消息: id={}, method={}", id, method);

        std::shared_ptr<HandlerSlot> slot;
        {
            std::shared_lock lock(m_mutex);
            auto it = m_handlers.find(method);
            if (it != m_handlers.end()) slot = it->second;
        }
        bool acquired = false;
        bool useRawHandler = false;
        if (slot) {
            std::lock_guard lock(slot->mutex);
            if (slot->accepting && (slot->handler || slot->rawHandler)) {
                ++slot->activeCalls;
                acquired = true;
                useRawHandler = static_cast<bool>(slot->rawHandler);
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
        if (useRawHandler) {
            std::string rawResult;
            try {
                rawResult = slot->rawHandler(params);
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
            return bridgeRawSuccessResponse(id, rawResult);
        }

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

        if (result.is_object() && result.size() == 1 && result.contains("__raw_json_result") &&
            result["__raw_json_result"].is_string()) {
            return bridgeRawSuccessResponse(
                id, result["__raw_json_result"].get_ref<const std::string&>());
        }

        json response = {
            {"id", id},
            {"result", result}
        };
        return response.dump(-1, ' ', false, json::error_handler_t::replace);
    } catch (const std::exception& e) {
        LOG_ERROR("IPC 处理器异常: id={}, error={}", id, e.what());
        json response = {
            {"id", id},
            {"error", {{"code", -32603}, {"message", std::string("Internal error: ") + e.what()}}}
        };
        return response.dump(-1, ' ', false, json::error_handler_t::replace);
    } catch (...) {
        LOG_ERROR("IPC 处理器未知异常: id={}", id);
        json response = {
            {"id", id},
            {"error", {{"code", -32603}, {"message", "Internal error: unknown exception"}}}
        };
        return response.dump(-1, ' ', false, json::error_handler_t::replace);
    }
}

// ── 异步方法线程池 ──────────────────────────────────────────────────────────
//
// 队列里只保存原始消息文本和响应回调，处理器是任务真正执行时才从 m_handlers
// 查找的。因此排队中的任务不会持有插件 DLL 内的 std::function，插件卸载不会
// 留下指向已卸载模块的悬空调用。

namespace {

// 8 线程异步池：避免慢速内容搜索与批量扩展属性提取占满槽位而饿死文件 OCR / 图标 / 设置等高频前台 IPC。
constexpr size_t AsyncWorkerCount = 8;

/// 队列上限。搜索场景下堆积几乎都来自连续击键，因此过载时丢弃最旧的一条，
/// 并立刻给它回一个错误响应，避免前端 Promise 悬挂到超时。
constexpr size_t MaxQueuedAsyncJobs = 64;
constexpr auto DefaultAsyncJobTimeout = std::chrono::seconds(30);
// 全文搜索会真实读取并解压大量文档。前端已经为 search.query 保留 30 分钟，
// 后端不能再用普通 IPC 的 30 秒熔断提前返回空结果。
constexpr auto SearchQueryAsyncJobTimeout = std::chrono::minutes(30);
// 弹出模态文件对话框或 UAC 提权确认框的异步方法等待用户交互，不能被 30 秒看门狗熔断。
constexpr auto InteractiveAsyncJobTimeout = std::chrono::minutes(30);

std::chrono::steady_clock::duration asyncJobTimeoutForMethod(std::string_view method) noexcept {
    if (method == "search.query") {
        return SearchQueryAsyncJobTimeout;
    }
    if (method == "config.export" ||
        method == "config.import" ||
        method == "capture.browseDirectory" ||
        method == "backup.browseDirectory" ||
        method == "system.browseFileLocksmithTarget" ||
        method == "app.exportLogs" ||
        method == "capture.saveHistoryItemAs" ||
        method == "system.pickForegroundWindow" ||
        method == "autoStart.set" ||
        method == "general.updateSettings" ||
        method == "app.restartElevated" ||
        method == "app.restartDemoted" ||
        method == "search.openFileAsAdmin" ||
        method == "system.openFileAsAdmin" ||
        method == "search.openAsAdmin" ||
        method == "system.openAsAdmin") {
        return InteractiveAsyncJobTimeout;
    }
    return DefaultAsyncJobTimeout;
}

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
        std::function<void()> internalTask;
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
                if (job.internalTask) {
                    try {
                        job.internalTask();
                    } catch (const std::exception& e) {
                        LOG_ERROR("Internal async worker task exception: {}", e.what());
                    } catch (...) {
                        LOG_ERROR("Internal async worker task unknown exception");
                    }
                } else {
                    std::string response = MessageBridge::instance().handleMessage(job.message);
                    WorkerPool::respondOnce(job.completion, std::move(response), "worker");
                }
                {
                    std::lock_guard lock(pool->mutex);
                    workerState->active = false;
                    workerState->timedOut = false;
                    workerState->completion.reset();
                }
                pool->watchdogCv.notify_all();
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

    // CRIT-03: 有界排空等待最高 3000ms 让在途工作线程自然完成并 join()；
    // 若在主 UI 线程则泵送已发送跨线程消息与 MainThreadDispatcher 队列防止 AB-BA 死锁，仅当极端超时仍卡死时才降级 detach()。
    constexpr DWORD ShutdownDrainBudgetMs = 3000;
    const bool onUiThread = MainThreadDispatcher::instance().isOwnerThread();
    if (!onUiThread) {
        std::unique_lock lock(pool->mutex);
        pool->watchdogCv.wait_for(
            lock, std::chrono::milliseconds(ShutdownDrainBudgetMs), [pool] {
                return std::none_of(pool->workerStates.begin(), pool->workerStates.end(),
                                    [](const auto& s) { return s->active; });
            });
    }

    const ULONGLONG deadlineTick = GetTickCount64() + ShutdownDrainBudgetMs;
    bool leakedForStalledWorkers = false;
    for (size_t i = 0; i < pool->threads.size(); ++i) {
        auto& thread = pool->threads[i];
        if (!thread.joinable()) continue;

        bool joinedCleanly = false;
        if (onUiThread) {
            MainThreadDispatcher::instance().drain();
            const ULONGLONG nowTick = GetTickCount64();
            const DWORD remainingMs = nowTick < deadlineTick
                ? static_cast<DWORD>(deadlineTick - nowTick)
                : 0;
            if (remainingMs > 0) {
                joinedCleanly = joinWorkerWhilePumpingSentMessages(thread, remainingMs);
            } else {
                HANDLE hThread = static_cast<HANDLE>(thread.native_handle());
                if (!hThread || WaitForSingleObject(hThread, 0) == WAIT_OBJECT_0) {
                    thread.join();
                    joinedCleanly = true;
                } else {
                    thread.detach();
                    joinedCleanly = false;
                }
            }
        } else {
            HANDLE hThread = static_cast<HANDLE>(thread.native_handle());
            const ULONGLONG nowTick = GetTickCount64();
            const DWORD remainingMs = nowTick < deadlineTick
                ? static_cast<DWORD>(deadlineTick - nowTick)
                : 0;
            if (!hThread || WaitForSingleObject(hThread, std::min<DWORD>(remainingMs, 50)) == WAIT_OBJECT_0) {
                thread.join();
                joinedCleanly = true;
            } else {
                thread.detach();
                joinedCleanly = false;
            }
        }

        if (!joinedCleanly) {
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
            }
            leakedForStalledWorkers = true;
            g_hasQuarantinedHandlers.store(true, std::memory_order_release);
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

    const size_t maxBytes = resolveBridgeMessageSizeLimit(messageJson);
    if (messageJson.size() > maxBytes) {
        const int rejectedId = extractFallbackRequestId(messageJson);
        LOG_WARN("拒绝过大的异步 IPC 消息: {} bytes", messageJson.size());
        responder(bridgeErrorResponse(rejectedId, -32600, "Request exceeds size limit"));
        return;
    }

    int id = 0;
    bool runAsync = false;
    std::string method;
    if (tryFastExtractJsonRpcHeader(messageJson, id, method)) {
        std::shared_lock lock(m_mutex);
        runAsync = m_asyncMethods.find(method) != m_asyncMethods.end();
    } else {
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
            const auto timeout = asyncJobTimeoutForMethod(method);
            pool.jobs.push_back(WorkerPool::Job{messageJson, completion, {}, timeout});
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
    return it->second->accepting &&
           (static_cast<bool>(it->second->handler) || static_cast<bool>(it->second->rawHandler));
}

json MessageBridge::invokeHandler(const std::string& method, const json& params) {
    std::shared_ptr<HandlerSlot> slot;
    {
        std::shared_lock lock(m_mutex);
        auto it = m_handlers.find(method);
        if (it != m_handlers.end()) slot = it->second;
    }
    if (!slot) return {{"error", "handler not found"}};
    bool useRawHandler = false;
    {
        std::lock_guard slotLock(slot->mutex);
        if (!slot->accepting || (!slot->handler && !slot->rawHandler)) {
            return {{"error", "handler not accepting"}};
        }
        ++slot->activeCalls;
        useRawHandler = static_cast<bool>(slot->rawHandler);
    }
    json result;
    try {
        if (useRawHandler) {
            const std::string raw = slot->rawHandler(params);
            result = raw.empty() ? json(nullptr) : json::parse(raw);
        } else {
            result = slot->handler(params);
            if (result.is_object() && result.size() == 1 &&
                result.contains("__raw_json_result") &&
                result["__raw_json_result"].is_string()) {
                result = json::parse(result["__raw_json_result"].get_ref<const std::string&>());
            }
        }
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

bool isCoreBuiltinPlugin(const std::string& id) {
    return id == "gesture" || id == "capture" || id == "search" ||
           id == "keycast" || id == "dialogenhancer" || id == "dialog_enhancer" ||
           id == "spotlight" || id == "clipboard" || id == "remote_boost" || id == "remote" ||
           id == "text_expander" || id == "textexpander" ||
           id == "picasa_viewer" || id == "picasaviewer" ||
           id == "file_locksmith" || id == "filelocksmith";
}

constexpr wchar_t PicasaViewerExeName[] = L"Tools3000_Picasa_Viewer.exe";
constexpr wchar_t PicasaViewerProgIdW[] = L"Tools3000.PicasaViewer";
constexpr wchar_t PicasaViewerFileAssocRegPath[] =
    L"Software\\Tools3000\\PicasaViewer\\Capabilities\\FileAssociations";
constexpr DWORD PicasaViewerCliTimeoutMs = 10000;

constexpr std::string_view PicasaViewerSupportedExtensions[] = {
    "jpg", "jpeg", "jpe", "jfif", "pjpeg",
    "png", "apng",
    "webp",
    "gif",
    "bmp", "dib", "rle",
    "ico", "cur",
    "tif", "tiff",
    "svg", "svgz",
    "avif", "heic", "heif", "hif",
    "wdp", "hdp", "jxr",
    "tga", "qoi", "ppm", "pgm", "pbm", "dds",
    "dng", "cr2", "cr3", "nef", "arw", "orf", "rw2", "pef", "raf", "srw",
};

std::filesystem::path getPicasaViewerExePath() {
    return WinUtils::getExeDirectory() / PicasaViewerExeName;
}

bool isSafePicasaViewerBinary(const std::filesystem::path& exePath) noexcept {
    if (exePath.empty()) return false;
    std::error_code ec;
    if (!std::filesystem::exists(exePath, ec) || !std::filesystem::is_regular_file(exePath, ec)) {
        return false;
    }
    const DWORD dirAttrs = GetFileAttributesW(exePath.parent_path().c_str());
    const DWORD fileAttrs = GetFileAttributesW(exePath.c_str());
    if (dirAttrs == INVALID_FILE_ATTRIBUTES || fileAttrs == INVALID_FILE_ATTRIBUTES) {
        return false;
    }
    return ((dirAttrs & FILE_ATTRIBUTE_REPARSE_POINT) == 0) &&
           ((fileAttrs & FILE_ATTRIBUTE_REPARSE_POINT) == 0);
}

std::wstring buildPicasaViewerLangArg() {
    const std::string lang = ConfigManager::instance().get<std::string>("/general/language", "auto");
    if (lang.empty() || lang == "auto" || lang == "system" || lang.size() > 16) {
        return L"";
    }
    for (unsigned char ch : lang) {
        const bool ok = (ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') ||
                        (ch >= '0' && ch <= '9') || ch == '-' || ch == '_';
        if (!ok) {
            return L"";
        }
    }
    return L"--lang " + WinUtils::utf8ToWstring(lang);
}

std::wstring readHkcuRegString(const std::wstring& subKey, const wchar_t* valueName) {
    HKEY hKey = nullptr;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, subKey.c_str(), 0, KEY_READ, &hKey) != ERROR_SUCCESS) {
        return L"";
    }
    wchar_t buf[260]{};
    DWORD bytes = static_cast<DWORD>(sizeof(buf));
    DWORD type = 0;
    const LONG st = RegQueryValueExW(
        hKey, valueName, nullptr, &type, reinterpret_cast<LPBYTE>(buf), &bytes);
    RegCloseKey(hKey);
    if (st != ERROR_SUCCESS || type != REG_SZ || bytes < sizeof(wchar_t)) {
        return L"";
    }
    const size_t chars = std::min<size_t>(bytes / sizeof(wchar_t), std::size(buf) - 1);
    buf[chars] = L'\0';
    return std::wstring(buf);
}

bool isPicasaViewerEffectiveDefaultForExt(const std::string& extLower) {
    const std::wstring dotExt = L"." + WinUtils::utf8ToWstring(extLower);
    const std::wstring userChoicePath =
        L"Software\\Microsoft\\Windows\\CurrentVersion\\Explorer\\FileExts\\" + dotExt + L"\\UserChoice";
    const std::wstring userChoiceProgId = readHkcuRegString(userChoicePath, L"ProgId");
    if (!userChoiceProgId.empty()) {
        return _wcsicmp(userChoiceProgId.c_str(), PicasaViewerProgIdW) == 0;
    }
    const std::wstring classesPath = L"Software\\Classes\\" + dotExt;
    const std::wstring defaultProgId = readHkcuRegString(classesPath, nullptr);
    return _wcsicmp(defaultProgId.c_str(), PicasaViewerProgIdW) == 0;
}

bool isExtLockedByOtherUserChoice(const std::string& extLower) {
    const std::wstring dotExt = L"." + WinUtils::utf8ToWstring(extLower);
    const std::wstring userChoicePath =
        L"Software\\Microsoft\\Windows\\CurrentVersion\\Explorer\\FileExts\\" + dotExt + L"\\UserChoice";
    const std::wstring userChoiceProgId = readHkcuRegString(userChoicePath, L"ProgId");
    return !userChoiceProgId.empty() && _wcsicmp(userChoiceProgId.c_str(), PicasaViewerProgIdW) != 0;
}

std::vector<std::string> queryPicasaViewerRegisteredExtensions() {
    std::vector<std::string> result;
    HKEY hKey = nullptr;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, PicasaViewerFileAssocRegPath, 0, KEY_READ, &hKey) != ERROR_SUCCESS) {
        return result;
    }

    std::unordered_set<std::string> dedup;
    wchar_t valueName[128]{};
    wchar_t valueData[256]{};
    for (DWORD index = 0;; ++index) {
        DWORD nameLen = static_cast<DWORD>(std::size(valueName));
        DWORD dataBytes = static_cast<DWORD>(sizeof(valueData));
        DWORD valueType = 0;
        const LONG status = RegEnumValueW(
            hKey, index, valueName, &nameLen, nullptr, &valueType,
            reinterpret_cast<LPBYTE>(valueData), &dataBytes);
        if (status == ERROR_NO_MORE_ITEMS) {
            break;
        }
        if (status != ERROR_SUCCESS || valueType != REG_SZ || dataBytes < sizeof(wchar_t)) {
            continue;
        }
        const size_t dataChars = std::min<size_t>(dataBytes / sizeof(wchar_t), std::size(valueData) - 1);
        valueData[dataChars] = L'\0';
        if (wcscmp(valueData, PicasaViewerProgIdW) != 0) {
            continue;
        }
        std::wstring extW(valueName, nameLen);
        if (!extW.empty() && extW.front() == L'.') {
            extW.erase(extW.begin());
        }
        std::string ext = WinUtils::wstringToUtf8(extW);
        std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char ch) {
            return static_cast<char>(std::tolower(ch));
        });
        if (!ext.empty() && dedup.insert(ext).second) {
            result.push_back(std::move(ext));
        }
    }
    RegCloseKey(hKey);
    std::sort(result.begin(), result.end());
    return result;
}

bool runPicasaViewerCliSync(const std::wstring& args, std::string& error) {
    const auto exePath = getPicasaViewerExePath();
    if (!isSafePicasaViewerBinary(exePath)) {
        error = "Tools3000_Picasa_Viewer.exe not found";
        return false;
    }

    std::wstring cmdLine = L"\"" + exePath.wstring() + L"\"";
    if (!args.empty()) {
        cmdLine += L" ";
        cmdLine += args;
    }

    STARTUPINFOW si{};
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi{};
    const BOOL created = CreateProcessW(
        exePath.c_str(),
        cmdLine.data(),
        nullptr,
        nullptr,
        FALSE,
        CREATE_NO_WINDOW,
        nullptr,
        exePath.parent_path().c_str(),
        &si,
        &pi);
    if (!created) {
        error = "CreateProcessW failed (" + std::to_string(GetLastError()) + ")";
        return false;
    }

    const DWORD waitResult = WaitForSingleObject(pi.hProcess, PicasaViewerCliTimeoutMs);
    DWORD exitCode = 1;
    if (waitResult == WAIT_OBJECT_0) {
        GetExitCodeProcess(pi.hProcess, &exitCode);
    } else {
        TerminateProcess(pi.hProcess, 1);
        error = "Picasa viewer CLI timed out";
    }
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);

    if (waitResult != WAIT_OBJECT_0) {
        return false;
    }
    if (exitCode != 0) {
        error = "Picasa viewer CLI exited with code " + std::to_string(exitCode);
        return false;
    }
    return true;
}

bool launchPicasaViewerDetached(const std::wstring& args, std::string& error) {
    const auto exePath = getPicasaViewerExePath();
    if (!isSafePicasaViewerBinary(exePath)) {
        error = "Tools3000_Picasa_Viewer.exe not found";
        return false;
    }

    std::wstring cmdLine = L"\"" + exePath.wstring() + L"\"";
    if (!args.empty()) {
        cmdLine += L" ";
        cmdLine += args;
    }

    STARTUPINFOW si{};
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi{};
    const BOOL created = CreateProcessW(
        exePath.c_str(),
        cmdLine.data(),
        nullptr,
        nullptr,
        FALSE,
        0,
        nullptr,
        exePath.parent_path().c_str(),
        &si,
        &pi);
    if (!created) {
        error = "CreateProcessW failed (" + std::to_string(GetLastError()) + ")";
        return false;
    }
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    return true;
}

bool getConfiguredPicasaViewerEnabled() {
    auto& cfg = ConfigManager::instance();
    if (cfg.has("/plugins/picasa_viewer/enabled")) {
        return cfg.get<bool>("/plugins/picasa_viewer/enabled", false);
    }
    if (cfg.has("/picasa_viewer/enabled")) {
        return cfg.get<bool>("/picasa_viewer/enabled", false);
    }
    return false;
}

bool reconcilePicasaViewerEnabledFromRegistry() {
    const auto exePath = getPicasaViewerExePath();
    const bool installed = isSafePicasaViewerBinary(exePath);
    const bool configured = getConfiguredPicasaViewerEnabled();
    if (!installed) {
        return false;
    }
    const auto registered = queryPicasaViewerRegisteredExtensions();
    const bool actualEnabled = !registered.empty();
    if (actualEnabled != configured) {
        ConfigManager::instance().set<bool>("/picasa_viewer/enabled", actualEnabled);
        ConfigManager::instance().set<bool>("/plugins/picasa_viewer/enabled", actualEnabled);
        return true;
    }
    return false;
}

json buildPicasaViewerStatusJson() {
    const auto exePath = getPicasaViewerExePath();
    const bool installed = isSafePicasaViewerBinary(exePath);
    const auto registered = queryPicasaViewerRegisteredExtensions();
    const bool configured = getConfiguredPicasaViewerEnabled();
    // 双向状态对账：以 HKCU 真实关联存在性为准收敛外部 --config/--register/--unregister 改动
    bool enabled = installed && configured;
    if (installed && ((!registered.empty()) != configured)) {
        enabled = !registered.empty();
        ConfigManager::instance().set<bool>("/picasa_viewer/enabled", enabled);
        ConfigManager::instance().set<bool>("/plugins/picasa_viewer/enabled", enabled);
    }

    std::vector<std::string> effectiveDefaults;
    size_t userChoiceLockedCount = 0;
    for (const auto& ext : registered) {
        if (isPicasaViewerEffectiveDefaultForExt(ext)) {
            effectiveDefaults.push_back(ext);
        } else if (isExtLockedByOtherUserChoice(ext)) {
            ++userChoiceLockedCount;
        }
    }

    json supportedArr = json::array();
    for (const auto ext : PicasaViewerSupportedExtensions) {
        supportedArr.push_back(std::string(ext));
    }
    const size_t supportedCount = std::size(PicasaViewerSupportedExtensions);
    return {
        {"enabled", enabled},
        {"installed", installed},
        {"exePath", WinUtils::wstringToUtf8(exePath.wstring())},
        {"registeredExtensions", registered},
        {"registeredCount", registered.size()},
        {"effectiveDefaultExtensions", effectiveDefaults},
        {"effectiveDefaultCount", effectiveDefaults.size()},
        {"userChoiceLockedCount", userChoiceLockedCount},
        {"supportedExtensions", std::move(supportedArr)},
        {"supportedCount", supportedCount},
        {"allAssociated", installed && registered.size() >= supportedCount},
    };
}

bool launchPicasaViewerConfigAndWatch(const std::wstring& args, std::string& error) {
    const auto exePath = getPicasaViewerExePath();
    if (!isSafePicasaViewerBinary(exePath)) {
        error = "Tools3000_Picasa_Viewer.exe not found";
        return false;
    }

    std::wstring cmdLine = L"\"" + exePath.wstring() + L"\"";
    if (!args.empty()) {
        cmdLine += L" ";
        cmdLine += args;
    }

    STARTUPINFOW si{};
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi{};
    const BOOL created = CreateProcessW(
        exePath.c_str(),
        cmdLine.data(),
        nullptr,
        nullptr,
        FALSE,
        0,
        nullptr,
        exePath.parent_path().c_str(),
        &si,
        &pi);
    if (!created) {
        error = "CreateProcessW failed (" + std::to_string(GetLastError()) + ")";
        return false;
    }
    CloseHandle(pi.hThread);
    HANDLE hProcess = pi.hProcess;
    std::thread([hProcess]() {
        WaitForSingleObject(hProcess, INFINITE);
        CloseHandle(hProcess);
        reconcilePicasaViewerEnabledFromRegistry();
        const auto status = buildPicasaViewerStatusJson();
        const bool nowEnabled = status.value("enabled", false);
        MessageBridge::instance().pushEvent("picasa_viewer:stateChanged", status);
        MessageBridge::instance().pushEvent("plugins:changed", {{"id", "picasa_viewer"}, {"enabled", nowEnabled}});
    }).detach();
    return true;
}

bool applyPicasaViewerEnabledState(bool enabled, std::string& error) {
    const auto exePath = getPicasaViewerExePath();
    const bool installed = isSafePicasaViewerBinary(exePath);
    if (enabled) {
        if (!installed) {
            error = "Tools3000_Picasa_Viewer.exe not found";
            LOG_ERROR("Picasa 图片查看器启用失败，无法完成文件关联: {}", error);
            return false;
        }
        if (!runPicasaViewerCliSync(L"--register-silent", error)) {
            LOG_ERROR("Picasa 图片查看器启用失败，无法完成文件关联: {}", error);
            return false;
        }
        ConfigManager::instance().set<bool>("/picasa_viewer/enabled", true);
        ConfigManager::instance().set<bool>("/plugins/picasa_viewer/enabled", true);
        const auto status = buildPicasaViewerStatusJson();
        LOG_INFO("Picasa 图片查看器已启用并自动关联全部支持的图片格式: count={}",
                 status.value("registeredCount", 0));
        MessageBridge::instance().pushEvent("picasa_viewer:stateChanged", status);
        MessageBridge::instance().pushEvent("plugins:changed", {{"id", "picasa_viewer"}, {"enabled", true}});
        return true;
    }

    if (installed) {
        std::string unregErr;
        if (!runPicasaViewerCliSync(L"--unregister-silent", unregErr)) {
            LOG_WARN("Picasa 图片查看器关闭时清理文件关联失败: {}", unregErr);
        }
    }
    ConfigManager::instance().set<bool>("/picasa_viewer/enabled", false);
    ConfigManager::instance().set<bool>("/plugins/picasa_viewer/enabled", false);
    const auto status = buildPicasaViewerStatusJson();
    LOG_INFO("Picasa 图片查看器已关闭并自动清理文件关联");
    MessageBridge::instance().pushEvent("picasa_viewer:stateChanged", status);
    MessageBridge::instance().pushEvent("plugins:changed", {{"id", "picasa_viewer"}, {"enabled", false}});
    return true;
}

void releasePluginResourcesAndTrim() {
    tools3000::core::WinUtils::trimWorkingSet();
}

void syncFullscreenRuntimeState() {
    HWND fg = GetForegroundWindow();
    const bool gestureBypassEnabled =
        ConfigManager::instance().get<bool>("/gesture/autoBypassFullscreen", true);
    const bool suspendHotkeys =
        fg && WinUtils::shouldSuspendHotkeysForWindow(fg);
    const bool suspendGestures =
        gestureBypassEnabled && fg && WinUtils::shouldSuspendGesturesForWindow(fg);

    auto& hotkeyMgr = HotkeyManager::instance();
    if (hotkeyMgr.isFullscreenSuspended() != suspendHotkeys) {
        hotkeyMgr.setFullscreenSuspended(suspendHotkeys);
    }

    auto& mouseHook = MouseHook::instance();
    if (mouseHook.isFullscreenSuspended() != suspendGestures) {
        mouseHook.setFullscreenSuspended(suspendGestures);
        if (suspendGestures) {
            EventBus::instance().publish(CancelTransientUiEvent{});
        }
    }
}

}  // namespace

void MessageBridge::registerBuiltinHandlers() {
    for (const char* method : {
             "plugins.setEnabled",
             "plugins.getAll",
             "app.restart",
             "app.restartElevated",
             "app.restartDemoted",
             "app.trimMemory",
             "app.installUpdate",
             "general.getSettings",
             "general.updateSettings",
             "autoStart.set",
             "config.export",
             "config.import",
             "config.reset",
             "capture.browseDirectory",
             "window.getOpenWindows",
             "picasaViewer.getStatus",
             "picasaViewer.setEnabled",
             "picasaViewer.associateAll",
             "picasaViewer.unassociateAll",
             "picasaViewer.openFormatConfig",
             "picasaViewer.openDefaultApps",
             "picasaViewer.launchViewer",
         }) {
        markMethodAsync(method);
    }

    // 后台异步预热开机自启状态原子缓存：投递至受管 WorkerPool 执行，杜绝裸 detach 线程逃逸生命周期管控
    static std::once_flag s_warmupOnce;
    std::call_once(s_warmupOnce, [this]() {
        auto& pool = ensureWorkerPool();
        bool enqueued = false;
        {
            std::lock_guard lock(pool.mutex);
            if (!pool.stopping) {
                WorkerPool::Job warmupJob;
                warmupJob.internalTask = []() { getCachedAutoStart(true); };
                warmupJob.timeout = DefaultAsyncJobTimeout;
                pool.jobs.push_back(std::move(warmupJob));
                enqueued = true;
            }
        }
        if (enqueued) {
            pool.cv.notify_one();
        }
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

        // 内置独立模块：Picasa 图片查看器 (Rust 复刻轻量版，默认关闭，开启自动关联全部支持图片格式)
        reconcilePicasaViewerEnabledFromRegistry();
        const auto picasaExePath = getPicasaViewerExePath();
        const bool picasaInstalled = isSafePicasaViewerBinary(picasaExePath);
        const bool picasaEnabled = picasaInstalled && getConfiguredPicasaViewerEnabled();
        plugins.push_back({
            {"id", "picasa_viewer"},
            {"name", "Picasa 图片查看器"},
            {"version", tools3000::version::String},
            {"fileName", "Tools3000_Picasa_Viewer.exe"},
            {"abiVersion", 1},
            {"capabilities", {"image-viewer", "file-associations", "slideshow", "high-dpi-d2d"}},
            {"permissions", {"hkcu-file-associations"}},
            {"executionModel", "standalone-rust-process"},
            {"enabled", picasaEnabled},
            {"active", picasaEnabled},
            {"restartRequired", false},
            {"state", !picasaInstalled ? "unavailable" : (picasaEnabled ? "running" : "disabled")},
            {"error", picasaInstalled ? "" : "Tools3000_Picasa_Viewer.exe not found"},
            {"isExtension", false}
        });
        existingIds.insert("picasa_viewer");

        // 内置核心模块：持久化剪贴板历史 (clipboard，Rust 持久化内核 + Win32 事件监听)
        const bool clipboardEnabled = ConfigManager::instance().has("/plugins/clipboard/enabled")
            ? ConfigManager::instance().get<bool>("/plugins/clipboard/enabled", true)
            : ConfigManager::instance().get<bool>("/clipboard/enabled", true);
        plugins.push_back({
            {"id", "clipboard"},
            {"name", "剪贴板历史"},
            {"version", tools3000::version::String},
            {"fileName", "Tools3000.exe"},
            {"abiVersion", 1},
            {"capabilities", {"persistent-history", "instant-search", "image-dedup", "privacy-filter"}},
            {"permissions", {"clipboard-listener", "input-synthesis"}},
            {"executionModel", "trusted-native-in-process"},
            {"enabled", clipboardEnabled},
            {"active", clipboardEnabled},
            {"restartRequired", false},
            {"state", clipboardEnabled ? "running" : "disabled"},
            {"error", ""},
            {"isExtension", false}
        });
        existingIds.insert("clipboard");

        // 内置一级模块：系统级文本扩展器 (text_expander，默认开启，关闭时零钩子零内存)
        const bool textExpanderEnabled = ConfigManager::instance().has("/plugins/text_expander/enabled")
            ? ConfigManager::instance().get<bool>("/plugins/text_expander/enabled", true)
            : ConfigManager::instance().get<bool>("/textExpander/enabled", true);
        plugins.push_back({
            {"id", "text_expander"},
            {"name", "Text Expander"},
            {"version", tools3000::version::String},
            {"fileName", "Tools3000.exe"},
            {"abiVersion", 1},
            {"capabilities", {"phrase-expansion", "dynamic-macros", "privacy-guard", "zero-idle-when-off"}},
            {"permissions", {"keyboard-hook", "input-synthesis"}},
            {"executionModel", "trusted-native-in-process"},
            {"enabled", textExpanderEnabled},
            {"active", textExpanderEnabled},
            {"restartRequired", false},
            {"state", textExpanderEnabled ? "running" : "disabled"},
            {"error", ""},
            {"isExtension", false}
        });
        existingIds.insert("text_expander");

        // 内置零常驻模块：文件占用解锁 (file_locksmith，纯 HKCU 右键菜单 + 按需原生解锁窗口，0 后台常驻内存)
        bool locksmithShiftOnly = false;
        const bool locksmithEnabled =
            FileLocksmithEngine::isExplorerContextMenuRegistered(&locksmithShiftOnly);
        plugins.push_back({
            {"id", "file_locksmith"},
            {"name", "文件占用解锁"},
            {"version", tools3000::version::String},
            {"fileName", "Tools3000.exe"},
            {"abiVersion", 1},
            {"capabilities", {"context-menu", "lock-inspector", "safe-unlock", "zero-resident"}},
            {"permissions", {"hkcu-context-menu"}},
            {"executionModel", "trusted-native-in-process"},
            {"enabled", locksmithEnabled},
            {"active", locksmithEnabled},
            {"restartRequired", false},
            {"state", locksmithEnabled ? "running" : "disabled"},
            {"error", ""},
            {"isExtension", false}
        });
        existingIds.insert("file_locksmith");
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

        if (id == "clipboard") {
            ConfigManager::instance().set<bool>("/clipboard/enabled", enabled);
            ConfigManager::instance().set<bool>("/plugins/clipboard/enabled", enabled);
            EventBus::instance().publish(ClipboardStateChangedEvent{enabled});
            MessageBridge::instance().pushEvent("clipboard:stateChanged", {{"enabled", enabled}});
            MessageBridge::instance().pushEvent("plugins:changed", {{"id", "clipboard"}, {"enabled", enabled}});
            releasePluginResourcesAndTrim();
            return {{"success", true}, {"restartRequired", false}};
        }

        if (id == "text_expander" || id == "textexpander") {
            ConfigManager::instance().set<bool>("/textExpander/enabled", enabled);
            ConfigManager::instance().set<bool>("/plugins/text_expander/enabled", enabled);
            if (MessageBridge::instance().hasHandler("textExpander.updateSettings")) {
                MessageBridge::instance().invokeHandler("textExpander.updateSettings", {{"enabled", enabled}});
            } else {
                MessageBridge::instance().pushEvent("textExpander:stateChanged", {{"enabled", enabled}});
                MessageBridge::instance().pushEvent("plugins:changed", {{"id", "text_expander"}, {"enabled", enabled}});
            }
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

        if (id == "file_locksmith" || id == "filelocksmith") {
            auto& cfg = ConfigManager::instance();
            bool regShiftOnly = false;
            const bool wasRegistered = FileLocksmithEngine::isExplorerContextMenuRegistered(&regShiftOnly);
            const bool shiftOnly = wasRegistered
                ? regShiftOnly
                : cfg.get<bool>("/general/shellMenuShiftOnly", true);
            cfg.set<bool>("/general/shellMenuLocksmithEnabled", enabled);
            cfg.set<bool>("/plugins/file_locksmith/enabled", enabled);
            const std::wstring verbLabel = tools3000::core::trW(
                L"使用 Tools3000 查看文件占用/解锁",
                L"Inspect File Locks with Tools3000");
            const bool ok = FileLocksmithEngine::syncExplorerContextMenu(
                enabled, shiftOnly, WinUtils::getExePath().wstring(), verbLabel);
            if (ok) {
                MessageBridge::instance().pushEvent("fileLocksmith:stateChanged", {
                    {"enabled", enabled},
                    {"shiftOnly", shiftOnly}
                });
                MessageBridge::instance().pushEvent("plugins:changed", {
                    {"id", "file_locksmith"},
                    {"enabled", enabled}
                });
            }
            return {{"success", ok}, {"restartRequired", false}};
        }

        if (id == "picasa_viewer" || id == "picasaviewer") {
            std::string error;
            const bool success = applyPicasaViewerEnabledState(enabled, error);
            if (success) {
                releasePluginResourcesAndTrim();
            }
            return {
                {"success", success},
                {"restartRequired", false},
                {"error", error},
                {"status", buildPicasaViewerStatusJson()}
            };
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
            if (success) {
                MessageBridge::instance().pushEvent("dialog:configChanged", {{"enabled", enabled}});
                MessageBridge::instance().pushEvent("plugins:changed", {{"id", normalizedId}, {"enabled", enabled}});
                releasePluginResourcesAndTrim();
            }
            return {
                {"success", success},
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
            if (success) {
                MessageBridge::instance().pushEvent("keycast:configChanged", {{"enabled", enabled}});
                MessageBridge::instance().pushEvent("plugins:changed", {{"id", "keycast"}, {"enabled", enabled}});
                releasePluginResourcesAndTrim();
            }
            return {
                {"success", success},
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
            if (success) {
                MessageBridge::instance().pushEvent("gesture.stateChanged", {{"enabled", enabled}, {"paused", !enabled}});
                MessageBridge::instance().pushEvent("gesture:stateChanged", {{"enabled", enabled}, {"paused", !enabled}});
                MessageBridge::instance().pushEvent("plugins:changed", {{"id", "gesture"}, {"enabled", enabled}});
                releasePluginResourcesAndTrim();
            }
            return {
                {"success", success},
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

    registerRawHostHandler("config.getAll", [](const json&) -> std::string {
        return ConfigManager::instance().toJsonString(-1);
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
        if (ok) {
            WinUtils::setFullscreenPolicyMode(WinUtils::parseFullscreenPolicyMode(
                ConfigManager::instance().get<std::string>("/general/fullscreenMode", "smart")));
            syncRuntimeSettingsFromConfig("import");
            syncFullscreenRuntimeState();
        }
        return {{"success", ok}};
    });
    registerHandler("config.reset", [](const json&) -> json {
        // 只有备份成功后才允许清空，避免磁盘异常时造成不可逆的数据丢失。
        if (!ConfigManager::instance().exportTo(
                WinUtils::getConfigDirectory() / "config_backup.json")) {
            return {{"success", false}, {"error", "failed to back up config"}};
        }
        const bool reset = ConfigManager::instance().reset();
        if (reset) {
            ConfigManager::instance().set("/general/autoStart", false);
            WinUtils::setFullscreenPolicyMode(FullscreenPolicyMode::Smart);
            syncRuntimeSettingsFromConfig("reset");
            syncFullscreenRuntimeState();
            LOG_INFO("配置已重置为默认值");
        }
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
        const auto parsedFullscreenMode = WinUtils::parseFullscreenPolicyMode(
            config.get<std::string>("/general/fullscreenMode", "smart"));
        WinUtils::setFullscreenPolicyMode(parsedFullscreenMode);
        return {
            {"autoStart", autoStart},
            {"runAsAdmin", config.get<bool>("/general/runAsAdmin", true)},
            {"elevated", WinUtils::isCurrentProcessElevated()},
            {"minimizeToTray", config.get<bool>("/general/minimizeToTray", true)},
            {"checkUpdates", config.get<bool>("/general/checkUpdates", true)},
            {"autoDownloadUpdates", config.get<bool>("/general/autoDownloadUpdates", true)},
            {"autoReleaseSettingsMemory", config.get<bool>("/general/autoReleaseSettingsMemory", true)},
            {"fullscreenMode", WinUtils::fullscreenPolicyModeToString(parsedFullscreenMode)},
            {"keycastEnabled", config.get<bool>("/general/keycastEnabled", true)},
            {"showOnboarding", config.get<bool>("/general/showOnboarding", false)},
            {"isPortableMode", WinUtils::isPortableMode()},
            {"dataDirectory", WinUtils::wstringToUtf8(WinUtils::getAppDataDirectory().wstring())},
            {"language", config.get<std::string>("/general/language", "auto")},
            {"systemLanguage", UiI18n::getSystemLanguageTag()},
            {"resolvedLanguage", UiI18n::getEffectiveLanguageTag()},
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
            "autoStart", "runAsAdmin", "minimizeToTray", "checkUpdates", "autoDownloadUpdates", "autoReleaseSettingsMemory", "keycastEnabled", "showOnboarding"
        };
        static const std::unordered_set<std::string> themes = {"system", "light", "dark"};
        static const std::unordered_set<std::string> trayIconThemes = {"system", "auto", "light", "dark", "color"};
        static const std::unordered_set<std::string> logLevels = {"trace", "debug", "info", "warn", "error"};
        static const std::unordered_set<std::string> fontFamilies = {
            "auto", "noto-sans-sc", "harmony-sans", "yahei", "pingfang", "system"
        };
        static const std::unordered_set<std::string> fullscreenModes = {
            "smart", "strict_all", "gesture_only", "off"
        };
        static const std::unordered_set<std::string> readOnlyKeys = {
            "elevated", "isPortableMode", "dataDirectory"
        };
        static const std::unordered_set<std::string> accents = {
            "violet", "cyan", "amber", "blue", "mint", "coral"
        };
        static const std::unordered_set<std::string> languages = {
            "auto", "system", "zh-CN", "zh-TW", "zh-HK", "en-US", "ja-JP", "ko-KR", "ru-RU", "pt-BR", "es-ES"
        };
        if (!params.is_object() || params.empty()) {
            return {{"success", false}, {"error", "no settings supplied"}};
        }
        auto& config = ConfigManager::instance();
        const bool previousAutoStart = config.get<bool>("/general/autoStart", false);
        json sanitizedPatch = json::object();
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
            if (key == "accentColor" &&
                (!value.is_string() ||
                 (!accents.contains(value.get<std::string>()) &&
                  !isValidHexColor(value.get<std::string>())))) {
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
            if (key == "fullscreenMode" && (!value.is_string() ||
                !fullscreenModes.contains(value.get<std::string>()))) {
                return {{"success", false}, {"error", "invalid fullscreen mode"}};
            }
            if (key == "logRetentionDays") {
                if (!value.is_number_integer() || value.get<int64_t>() < 0 || value.get<int64_t>() > 3650) {
                    return {{"success", false}, {"error", "invalid log retention days"}};
                }
            }
            if (!boolKeys.contains(key) && key != "theme" && key != "accentColor" && key != "logLevel" && key != "language" && key != "fontFamily" && key != "trayIconTheme" && key != "logRetentionDays" && key != "fullscreenMode") {
                return {{"success", false}, {"error", "unsupported setting: " + key}};
            }
            sanitizedPatch[key] = value;
        }
        if (sanitizedPatch.empty()) {
            return {{"success", true}};
        }
        if (sanitizedPatch.contains("autoStart") && !setAutoStart(sanitizedPatch["autoStart"].get<bool>())) {
            return {{"success", false}, {"error", "failed to update auto-start"}};
        }
        if (sanitizedPatch.contains("showOnboarding")) {
            const bool showOb = sanitizedPatch["showOnboarding"].get<bool>();
            config.set<bool>("/app/onboardingCompleted", !showOb);
        }
        const bool saved = config.mergePatch({{"general", sanitizedPatch}}, "/general");
        if (!saved) {
            if (sanitizedPatch.contains("autoStart")) setAutoStart(previousAutoStart);
            return {{"success", false}, {"error", "failed to persist settings"}};
        }
        if (sanitizedPatch.contains("fullscreenMode")) {
            WinUtils::setFullscreenPolicyMode(
                WinUtils::parseFullscreenPolicyMode(sanitizedPatch["fullscreenMode"].get<std::string>()));
            syncFullscreenRuntimeState();
        }
        if (sanitizedPatch.contains("language")) {
            const std::string newLang = sanitizedPatch["language"].get<std::string>();
            UiI18n::setConfiguredLanguage(newLang);
            Logger::setLanguage(newLang);
        }
        if (sanitizedPatch.contains("logLevel")) applyLogLevel(sanitizedPatch["logLevel"].get<std::string>());
        if (sanitizedPatch.contains("logRetentionDays")) {
            Logger::setRetentionDays(sanitizedPatch["logRetentionDays"].get<uint32_t>());
        }
        if (sanitizedPatch.contains("theme") || sanitizedPatch.contains("accentColor") || sanitizedPatch.contains("trayIconTheme")) {
            const std::string effectiveTheme = config.get<std::string>("/general/theme", "system");
            WinUtils::applyNativeMenuTheme(effectiveTheme);
            EventBus::instance().publish(ThemeChangedEvent{
                effectiveTheme,
                config.get<std::string>("/general/accentColor", "blue"),
                config.get<std::string>("/general/trayIconTheme", "system")
            });
        }
        if (sanitizedPatch.contains("theme") || sanitizedPatch.contains("accentColor") ||
            sanitizedPatch.contains("trayIconTheme") || sanitizedPatch.contains("language") ||
            sanitizedPatch.contains("fontFamily")) {
            MessageBridge::instance().pushEvent("appearance:changed", {
                {"theme", config.get<std::string>("/general/theme", "system")},
                {"accentColor", config.get<std::string>("/general/accentColor", "blue")},
                {"trayIconTheme", config.get<std::string>("/general/trayIconTheme", "system")},
                {"language", config.get<std::string>("/general/language", "auto")},
                {"systemLanguage", UiI18n::getSystemLanguageTag()},
                {"resolvedLanguage", UiI18n::getEffectiveLanguageTag()},
                {"fontFamily", config.get<std::string>("/general/fontFamily", "auto")}
            });
        }
        return {{"success", true}};
    });
    registerAsyncHostHandler("capture.browseDirectory", [](const json&) -> json {
        const auto selected = choosePath(false, true);
        return selected ? json(WinUtils::wstringToUtf8(selected->wstring())) : json(nullptr);
    });
    registerAsyncHostHandler("capture.openSaveDirectory", [](const json& params) -> json {
        auto trimWs = [](std::string s) {
            while (!s.empty() && static_cast<unsigned char>(s.front()) <= 0x20) {
                s.erase(s.begin());
            }
            while (!s.empty() && static_cast<unsigned char>(s.back()) <= 0x20) {
                s.pop_back();
            }
            return s;
        };

        const std::string kind = params.value("kind", "capture");
        std::string dirUtf8 = trimWs(params.value("path", ""));
        auto& cfg = ConfigManager::instance();
        if (dirUtf8.empty()) {
            if (kind == "recording") {
                dirUtf8 = trimWs(cfg.get<std::string>("/recording/saveDirectory", ""));
                if (dirUtf8.empty()) {
                    dirUtf8 = trimWs(cfg.get<std::string>("/recording/savePath", ""));
                }
            } else {
                dirUtf8 = trimWs(cfg.get<std::string>("/capture/saveDirectory", ""));
                if (dirUtf8.empty()) {
                    dirUtf8 = trimWs(cfg.get<std::string>("/capture/savePath", ""));
                }
            }
        }

        std::filesystem::path targetDir;
        if (!dirUtf8.empty()) {
            targetDir = std::filesystem::path(WinUtils::utf8ToWstring(dirUtf8));
        } else if (kind == "recording") {
            targetDir = WinUtils::getAppDataDirectory() / L"Recordings";
        } else {
            targetDir = WinUtils::getAppDataDirectory() / L"Screenshots";
        }

        std::error_code ec;
        std::filesystem::create_directories(targetDir, ec);
        const std::string resolvedUtf8 = WinUtils::wstringToUtf8(targetDir.wstring());
        if (ec && !std::filesystem::exists(targetDir, ec)) {
            return {
                {"success", false},
                {"path", resolvedUtf8},
                {"error", tools3000::core::trUtf8Cstr("无法创建保存目录", "Failed to create save directory")}
            };
        }

        if (params.value("dryRun", false)) {
            return {{"success", true}, {"path", resolvedUtf8}};
        }

        const HINSTANCE hInst = ShellExecuteW(
            nullptr, L"open", targetDir.wstring().c_str(), nullptr, nullptr, SW_SHOWNORMAL);
        const bool ok = reinterpret_cast<INT_PTR>(hInst) > 32;
        if (!ok) {
            return {
                {"success", false},
                {"path", resolvedUtf8},
                {"error", tools3000::core::trUtf8Cstr("无法打开保存目录", "Failed to open save directory")}
            };
        }
        return {{"success", true}, {"path", resolvedUtf8}};
    });
    registerSystemInteractionHandlers(*this);
    registerHostHandler("app.checkForUpdates", [](const json&) -> json {
        const bool started = UpdateChecker::instance().checkAsync(true);
        return {{"success", true}, {"started", started}};
    });
    registerHostHandler("app.getUpdateState", [](const json&) -> json {
        auto state = json::parse(UpdateChecker::instance().getStateJsonString(), nullptr, false);
        if (!state.is_object()) state = json::object();
        state["success"] = true;
        return state;
    });
    registerHostHandler("app.downloadUpdate", [](const json&) -> json {
        const bool started = UpdateChecker::instance().downloadUpdateAsync();
        auto state = json::parse(UpdateChecker::instance().getStateJsonString(), nullptr, false);
        if (!state.is_object()) state = json::object();
        state["success"] = true;
        state["started"] = started;
        return state;
    });
    registerHostHandler("app.installUpdate", [](const json&) -> json {
        std::string err;
        const bool ok = UpdateChecker::instance().installDownloadedUpdate(err);
        return {{"success", ok}, {"cancelled", err == "cancelled"}, {"error", err}};
    });
    registerHostHandler("app.dismissUpdate", [](const json&) -> json {
        const bool dismissed = UpdateChecker::instance().dismissCurrentUpdate();
        auto state = json::parse(UpdateChecker::instance().getStateJsonString(), nullptr, false);
        if (!state.is_object()) state = json::object();
        state["success"] = true;
        state["dismissedAction"] = dismissed;
        return state;
    });

    // ── 配置云备份与恢复 (同步盘 OneDrive/Google Drive · WebDAV · S3 SigV4) ───
    registerAsyncHostHandler("backup.browseDirectory", [](const json&) -> json {
        const auto selected = choosePath(false, true);
        return selected ? json(WinUtils::wstringToUtf8(selected->wstring())) : json(nullptr);
    });

    registerAsyncHostHandler("backup.getSettings", [](const json&) -> json {
        using tools3000::core::backup::ConfigBackupService;
        const auto s = ConfigBackupService::instance().getSettings();
        const auto folders = ConfigBackupService::detectLocalSyncFolders();
        return {
            {"success", true},
            {"settings", ConfigBackupService::settingsToJson(s)},
            {"detectedFolders", ConfigBackupService::detectedFoldersToJson(folders)},
        };
    });

    registerAsyncHostHandler("backup.saveSettings", [](const json& params) -> json {
        using namespace tools3000::core::backup;
        if (!params.is_object()) {
            return {{"success", false}, {"errorCode", "invalid_params"}};
        }
        auto current = ConfigBackupService::instance().getSettings();
        CloudBackupSecretUpdates secrets{};

        if (params.contains("enabled") && params["enabled"].is_boolean()) current.enabled = params["enabled"].get<bool>();
        if (params.contains("provider") && params["provider"].is_string()) {
            const auto p = ConfigBackupService::providerFromString(params["provider"].get<std::string>());
            if (p != BackupProviderType::None) current.provider = p;
        }
        if (params.contains("autoBackupOnExit") && params["autoBackupOnExit"].is_boolean()) {
            current.autoBackupOnExit = params["autoBackupOnExit"].get<bool>();
        }
        if (params.contains("keepCount") && params["keepCount"].is_number_integer()) {
            current.keepCount = params["keepCount"].get<int>();
        }
        if (params.contains("encryptionEnabled") && params["encryptionEnabled"].is_boolean()) {
            current.encryptionEnabled = params["encryptionEnabled"].get<bool>();
        }
        if (params.contains("folderPath") && params["folderPath"].is_string()) {
            current.folderPath = params["folderPath"].get<std::string>();
        }
        if (params.contains("webdavUrl") && params["webdavUrl"].is_string()) {
            current.webdavUrl = params["webdavUrl"].get<std::string>();
        }
        if (params.contains("webdavUsername") && params["webdavUsername"].is_string()) {
            current.webdavUsername = params["webdavUsername"].get<std::string>();
        }
        if (params.value("clearWebdavPassword", false)) {
            secrets.updateWebdavPassword = true;
            secrets.webdavPassword.clear();
        } else if (params.contains("webdavPassword") && params["webdavPassword"].is_string() &&
                   !params["webdavPassword"].get<std::string>().empty()) {
            secrets.updateWebdavPassword = true;
            secrets.webdavPassword = params["webdavPassword"].get<std::string>();
        }
        if (params.contains("s3Endpoint") && params["s3Endpoint"].is_string()) {
            current.s3Endpoint = params["s3Endpoint"].get<std::string>();
        }
        if (params.contains("s3Region") && params["s3Region"].is_string()) {
            current.s3Region = params["s3Region"].get<std::string>();
        }
        if (params.contains("s3Bucket") && params["s3Bucket"].is_string()) {
            current.s3Bucket = params["s3Bucket"].get<std::string>();
        }
        if (params.contains("s3Prefix") && params["s3Prefix"].is_string()) {
            current.s3Prefix = params["s3Prefix"].get<std::string>();
        }
        if (params.contains("s3AccessKeyId") && params["s3AccessKeyId"].is_string()) {
            current.s3AccessKeyId = params["s3AccessKeyId"].get<std::string>();
        }
        if (params.contains("s3ForcePathStyle") && params["s3ForcePathStyle"].is_boolean()) {
            current.s3ForcePathStyle = params["s3ForcePathStyle"].get<bool>();
        }
        if (params.value("clearS3SecretKey", false)) {
            secrets.updateS3SecretKey = true;
            secrets.s3SecretKey.clear();
        } else if (params.contains("s3SecretKey") && params["s3SecretKey"].is_string() &&
                   !params["s3SecretKey"].get<std::string>().empty()) {
            secrets.updateS3SecretKey = true;
            secrets.s3SecretKey = params["s3SecretKey"].get<std::string>();
        }
        if (params.value("clearEncryptionPassphrase", false)) {
            secrets.updateEncryptionPassphrase = true;
            secrets.encryptionPassphrase.clear();
        } else if (params.contains("encryptionPassphrase") && params["encryptionPassphrase"].is_string() &&
                   !params["encryptionPassphrase"].get<std::string>().empty()) {
            secrets.updateEncryptionPassphrase = true;
            secrets.encryptionPassphrase = params["encryptionPassphrase"].get<std::string>();
        }

        const bool ok = ConfigBackupService::instance().saveSettings(current, secrets);
        const auto updated = ConfigBackupService::instance().getSettings();
        return {
            {"success", ok},
            {"settings", ConfigBackupService::settingsToJson(updated)},
        };
    });

    registerAsyncHostHandler("backup.testConnection", [](const json& params) -> json {
        using tools3000::core::backup::ConfigBackupService;
        const json* draft = params.is_object() && !params.empty() ? &params : nullptr;
        const auto res = ConfigBackupService::instance().testConnection(draft);
        return ConfigBackupService::resultToJson(res);
    });

    registerAsyncHostHandler("backup.backupNow", [](const json&) -> json {
        using tools3000::core::backup::ConfigBackupService;
        const auto res = ConfigBackupService::instance().backupNow();
        json out = ConfigBackupService::resultToJson(res);
        out["settings"] = ConfigBackupService::settingsToJson(ConfigBackupService::instance().getSettings());
        return out;
    });

    registerAsyncHostHandler("backup.listSnapshots", [](const json& params) -> json {
        using tools3000::core::backup::ConfigBackupService;
        const json* draft = params.is_object() && !params.empty() ? &params : nullptr;
        const auto res = ConfigBackupService::instance().listSnapshots(draft);
        return ConfigBackupService::resultToJson(res);
    });

    registerAsyncHostHandler("backup.restoreSnapshot", [](const json& params) -> json {
        using tools3000::core::backup::ConfigBackupService;
        const std::string snapshotId = params.is_object() ? params.value("snapshotId", "") : "";
        const std::string passphrase = params.is_object() ? params.value("passphrase", "") : "";
        if (snapshotId.empty()) {
            return {{"success", false}, {"errorCode", "invalid_snapshot_id"}};
        }
        const auto res = ConfigBackupService::instance().restoreSnapshot(snapshotId, passphrase);
        return ConfigBackupService::resultToJson(res);
    });
    // ── 应用系统信息 ─────────────────────────────────────────────────────
    registerHostHandler("app.getSystemInfo", [](const json&) -> json {
        SYSTEM_INFO si{};
        GetNativeSystemInfo(&si);

        MEMORYSTATUSEX memInfo{};
        memInfo.dwLength = sizeof(memInfo);
        GlobalMemoryStatusEx(&memInfo);

        std::string arch = "x64";
        USHORT processMachine = IMAGE_FILE_MACHINE_UNKNOWN;
        USHORT nativeMachine = IMAGE_FILE_MACHINE_UNKNOWN;
        if (IsWow64Process2(GetCurrentProcess(), &processMachine, &nativeMachine) &&
            nativeMachine != IMAGE_FILE_MACHINE_UNKNOWN) {
            if (nativeMachine == IMAGE_FILE_MACHINE_ARM64) {
                arch = "ARM64";
            } else if (nativeMachine == IMAGE_FILE_MACHINE_AMD64) {
                arch = "x64";
            } else if (nativeMachine == IMAGE_FILE_MACHINE_I386) {
                arch = "x86";
            }
        } else if (si.wProcessorArchitecture == PROCESSOR_ARCHITECTURE_ARM64) {
            arch = "ARM64";
        } else if (si.wProcessorArchitecture == PROCESSOR_ARCHITECTURE_INTEL) {
            arch = "x86";
        }

        return {
            {"version", UpdateChecker::CurrentVersion},
            {"buildDate", tools3000::version::BuildDate},
            {"cpuArch", arch},
            {"cpuCores", si.dwNumberOfProcessors},
            {"totalMemoryGB", memInfo.ullTotalPhys / (1024.0 * 1024.0 * 1024.0)},
            {"dpiScale", WinUtils::getDpiScale()},
            {"language", WinUtils::isSystemLanguageChinese() ? "zh-CN" : "en-US"}
        };
    });

    // ── 打开的窗口与进程枚举 ─────────────────────────────────────────────
    registerHostHandler("window.getOpenWindows", [](const json&) -> json {
        struct WindowItem {
            std::string title;
            std::string processName;
            std::string windowClass;
            DWORD pid = 0;
        };

        std::vector<WindowItem> windows;
        EnumWindows([](HWND hwnd, LPARAM lParam) -> BOOL {
            if (!IsWindowVisible(hwnd)) return TRUE;

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

    registerHostHandler("window.getForegroundInfo", [](const json&) -> json {
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

    registerHostHandler("picasaViewer.getStatus", [](const json&) -> json {
        return buildPicasaViewerStatusJson();
    });

    registerHostHandler("picasaViewer.setEnabled", [](const json& params) -> json {
        if (!params.is_object() || !params.contains("enabled") || !params["enabled"].is_boolean()) {
            return {{"success", false}, {"error", "enabled is required"}};
        }
        std::string error;
        const bool ok = applyPicasaViewerEnabledState(params["enabled"].get<bool>(), error);
        if (ok) {
            releasePluginResourcesAndTrim();
        }
        return {
            {"success", ok},
            {"error", error},
            {"status", buildPicasaViewerStatusJson()}
        };
    });

    registerHostHandler("picasaViewer.associateAll", [](const json&) -> json {
        std::string error;
        if (!runPicasaViewerCliSync(L"--register-silent", error)) {
            LOG_ERROR("Picasa 图片查看器启用失败，无法完成文件关联: {}", error);
            return {{"success", false}, {"error", error}, {"status", buildPicasaViewerStatusJson()}};
        }
        ConfigManager::instance().set<bool>("/picasa_viewer/enabled", true);
        ConfigManager::instance().set<bool>("/plugins/picasa_viewer/enabled", true);
        const auto status = buildPicasaViewerStatusJson();
        LOG_INFO("Picasa 图片查看器已重新关联全部支持的图片格式: count={}",
                 status.value("registeredCount", 0));
        MessageBridge::instance().pushEvent("picasa_viewer:stateChanged", status);
        MessageBridge::instance().pushEvent("plugins:changed", {{"id", "picasa_viewer"}, {"enabled", true}});
        return {{"success", true}, {"status", status}};
    });

    registerHostHandler("picasaViewer.unassociateAll", [](const json&) -> json {
        std::string error;
        if (!runPicasaViewerCliSync(L"--unregister-silent", error)) {
            LOG_WARN("Picasa 图片查看器关闭时清理文件关联失败: {}", error);
            return {{"success", false}, {"error", error}, {"status", buildPicasaViewerStatusJson()}};
        }
        ConfigManager::instance().set<bool>("/picasa_viewer/enabled", false);
        ConfigManager::instance().set<bool>("/plugins/picasa_viewer/enabled", false);
        const auto status = buildPicasaViewerStatusJson();
        MessageBridge::instance().pushEvent("picasa_viewer:stateChanged", status);
        MessageBridge::instance().pushEvent("plugins:changed", {{"id", "picasa_viewer"}, {"enabled", false}});
        return {{"success", true}, {"status", status}};
    });

    registerHostHandler("picasaViewer.openFormatConfig", [](const json&) -> json {
        std::wstring args = L"--config";
        const std::wstring langArg = buildPicasaViewerLangArg();
        if (!langArg.empty()) {
            args += L" " + langArg;
        }
        std::string error;
        const bool ok = launchPicasaViewerConfigAndWatch(args, error);
        return {{"success", ok}, {"error", error}};
    });

    registerHostHandler("picasaViewer.openDefaultApps", [](const json&) -> json {
        HINSTANCE hInst = ShellExecuteW(
            nullptr, L"open",
            L"ms-settings:defaultapps?registeredAppUser=Tools3000%20Picasa%20Viewer",
            nullptr, nullptr, SW_SHOWNORMAL);
        if (reinterpret_cast<INT_PTR>(hInst) <= 32) {
            hInst = ShellExecuteW(
                nullptr, L"open", L"ms-settings:defaultapps", nullptr, nullptr, SW_SHOWNORMAL);
        }
        const bool ok = reinterpret_cast<INT_PTR>(hInst) > 32;
        return {{"success", ok}, {"error", ok ? "" : "failed to open Windows default apps settings"}};
    });

    registerHostHandler("picasaViewer.launchViewer", [](const json&) -> json {
        const std::wstring langArg = buildPicasaViewerLangArg();
        std::string error;
        const bool ok = launchPicasaViewerDetached(langArg, error);
        return {{"success", ok}, {"error", error}};
    });

    LOG_INFO("内置核心 IPC 处理器注册完成（含性能监控、配置管理、系统信息、窗口枚举）");
}

}  // namespace tools3000::core
