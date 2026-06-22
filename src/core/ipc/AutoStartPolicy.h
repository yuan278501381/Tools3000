#pragma once

#include <algorithm>
#include <cwctype>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <windows.h>
#include <lmcons.h>

#include <taskschd.h>
#include <comdef.h>

#pragma comment(lib, "taskschd.lib")
#pragma comment(lib, "comsupp.lib")

namespace tools3000::core::autostart {

inline std::wstring getCurrentUserName() {
    wchar_t username[UNLEN + 1] = {0};
    DWORD size = UNLEN + 1;
    if (GetUserNameW(username, &size) && size > 1) {
        return std::wstring(username);
    }
    wchar_t envBuf[256] = {0};
    if (GetEnvironmentVariableW(L"USERNAME", envBuf, 256) > 0) {
        return std::wstring(envBuf);
    }
    return L"CurrentUser";
}

inline constexpr wchar_t TASK_FOLDER_NAME[] = L"Tools3000";

inline std::wstring getTaskFolderName() {
    return L"\\" + std::wstring(TASK_FOLDER_NAME);
}

inline std::wstring getTaskName() {
    return L"Autorun for " + getCurrentUserName();
}

inline std::wstring getFullTaskPath() {
    return getTaskFolderName() + L"\\" + getTaskName();
}

inline std::wstring getAutoStartTaskName() {
    return getFullTaskPath();
}

inline void replaceAll(std::wstring& value, std::wstring_view from, std::wstring_view to);
inline std::wstring normalizeExecutablePath(std::wstring value);

inline bool isTestMode() {
    wchar_t buf[32]{};
    return GetEnvironmentVariableW(L"TOOLS3000_TEST_MODE", buf, 32) > 0 && wcscmp(buf, L"1") == 0;
}

/// 判断可执行文件是否处于开发编译目录（防止日常开发构建覆盖正式安装版开机自启）
inline bool isDevelopmentBinary(const std::filesystem::path& exePath) {
    std::wstring pathStr = normalizeExecutablePath(exePath.wstring());
    return pathStr.find(L"\\build\\") != std::wstring::npos ||
           pathStr.find(L"\\out\\") != std::wstring::npos ||
           pathStr.find(L"\\cmake-build-") != std::wstring::npos;
}

/// 判断可执行文件是否为非生产环境二进制 (单元测试套件或临时开发产物)
inline bool isNonProductionBinary(const std::filesystem::path& exePath) {
    std::wstring name = exePath.filename().wstring();
    std::transform(name.begin(), name.end(), name.begin(), ::towlower);
    if (name.find(L"test") != std::wstring::npos) return true;
    return isDevelopmentBinary(exePath);
}

/// 获取系统已安装的官方正式版可执行文件路径
inline std::optional<std::filesystem::path> getInstalledProductionExecutable() {
    const wchar_t* programFiles = _wgetenv(L"ProgramFiles");
    if (programFiles) {
        std::filesystem::path prodPath = std::filesystem::path(programFiles) / L"Tools3000" / L"Tools3000.exe";
        std::error_code ec;
        if (std::filesystem::exists(prodPath, ec)) {
            return prodPath;
        }
    }
    return std::nullopt;
}

struct ScopedTaskComGuard {
    bool needUninit = false;
    bool ready = false;

    ScopedTaskComGuard() noexcept {
        const HRESULT hr = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
        if (SUCCEEDED(hr)) {
            needUninit = true;
            ready = true;
        } else if (hr == RPC_E_CHANGED_MODE) {
            ready = true;
        }
    }

    ~ScopedTaskComGuard() noexcept {
        if (needUninit) {
            CoUninitialize();
        }
    }

    ScopedTaskComGuard(const ScopedTaskComGuard&) = delete;
    ScopedTaskComGuard& operator=(const ScopedTaskComGuard&) = delete;
};

inline bool isTaskNotFoundHr(HRESULT hr) noexcept {
    return hr == HRESULT_FROM_WIN32(ERROR_FILE_NOT_FOUND) ||
           hr == HRESULT_FROM_WIN32(ERROR_PATH_NOT_FOUND);
}

/// 注册 Windows 计划任务（通过 Task Scheduler 2.0 原生 COM 接口，100% 对齐 PixPin 配置）
inline bool registerTaskCOM(const std::wstring& exePath) {
    // 1. 测试模式全局熔断：严禁单元测试向宿主操作系统 TaskScheduler 注入物理计划任务
    if (isTestMode()) {
        return true;
    }

    // 2. 生产路径守卫：若试图注册测试二进制或临时构建产物，自动回退探测已安装的正式版
    std::wstring targetExe = exePath;
    if (isNonProductionBinary(std::filesystem::path(exePath))) {
        if (const auto prodExe = getInstalledProductionExecutable()) {
            targetExe = prodExe->wstring();
        } else {
            // 在未安装正式版的测试机器/纯开发环境，绝不将测试可执行文件物理写入任务计划
            return false;
        }
    }

    ScopedTaskComGuard comGuard;
    if (!comGuard.ready) {
        return false;
    }

    ITaskService* pService = nullptr;
    ITaskFolder* pRootFolder = nullptr;
    ITaskFolder* pToolsFolder = nullptr;
    ITaskDefinition* pTask = nullptr;
    IRegisteredTask* pRegisteredTask = nullptr;

    auto releaseAll = [&]() noexcept {
        if (pRegisteredTask) { pRegisteredTask->Release(); pRegisteredTask = nullptr; }
        if (pTask) { pTask->Release(); pTask = nullptr; }
        if (pToolsFolder) { pToolsFolder->Release(); pToolsFolder = nullptr; }
        if (pRootFolder) { pRootFolder->Release(); pRootFolder = nullptr; }
        if (pService) { pService->Release(); pService = nullptr; }
    };

    try {
        HRESULT hr = CoCreateInstance(CLSID_TaskScheduler, nullptr, CLSCTX_INPROC_SERVER,
                                      IID_ITaskService, reinterpret_cast<void**>(&pService));
        if (FAILED(hr) || !pService) {
            releaseAll();
            return false;
        }

        hr = pService->Connect(_variant_t(), _variant_t(), _variant_t(), _variant_t());
        if (FAILED(hr)) {
            releaseAll();
            return false;
        }

        // 1. 获取根目录文件夹
        hr = pService->GetFolder(_bstr_t(L"\\"), &pRootFolder);
        if (FAILED(hr) || !pRootFolder) {
            releaseAll();
            return false;
        }

        // 2. 创建或获取 \Tools3000 文件夹
        hr = pRootFolder->GetFolder(_bstr_t(L"Tools3000"), &pToolsFolder);
        if (FAILED(hr)) {
            hr = pRootFolder->CreateFolder(_bstr_t(L"Tools3000"), _variant_t(), &pToolsFolder);
        }
        pRootFolder->Release();
        pRootFolder = nullptr;

        if (FAILED(hr) || !pToolsFolder) {
            releaseAll();
            return false;
        }

        // 3. 创建任务定义
        hr = pService->NewTask(0, &pTask);
        if (FAILED(hr) || !pTask) {
            releaseAll();
            return false;
        }

        const std::wstring currentUser = getCurrentUserName();

        // 注册信息 (RegistrationInfo)
        IRegistrationInfo* pRegInfo = nullptr;
        if (SUCCEEDED(pTask->get_RegistrationInfo(&pRegInfo)) && pRegInfo) {
            try {
                pRegInfo->put_Author(_bstr_t(currentUser.c_str()));
                pRegInfo->put_Description(_bstr_t(L"Tools3000 桌面效率工具开机自启动任务"));
            } catch (const _com_error&) {}
            pRegInfo->Release();
        }

        // 主体与最高权限 (Principal: HighestAvailable, InteractiveToken)
        IPrincipal* pPrincipal = nullptr;
        if (SUCCEEDED(pTask->get_Principal(&pPrincipal)) && pPrincipal) {
            try {
                pPrincipal->put_Id(_bstr_t(L"Principal1"));
                pPrincipal->put_UserId(_bstr_t(currentUser.c_str()));
                pPrincipal->put_LogonType(TASK_LOGON_INTERACTIVE_TOKEN);
                pPrincipal->put_RunLevel(TASK_RUNLEVEL_HIGHEST);
            } catch (const _com_error&) {}
            pPrincipal->Release();
        }

        // 设置 (Settings: PT0S 无限生命周期, 允许电池启动, IgnoreNew 单实例)
        ITaskSettings* pSettings = nullptr;
        if (SUCCEEDED(pTask->get_Settings(&pSettings)) && pSettings) {
            try {
                pSettings->put_MultipleInstances(TASK_INSTANCES_IGNORE_NEW);
                pSettings->put_DisallowStartIfOnBatteries(VARIANT_FALSE);
                pSettings->put_StopIfGoingOnBatteries(VARIANT_FALSE);
                pSettings->put_ExecutionTimeLimit(_bstr_t(L"PT0S"));
                pSettings->put_AllowHardTerminate(VARIANT_TRUE);
                pSettings->put_StartWhenAvailable(VARIANT_FALSE);
                pSettings->put_AllowDemandStart(VARIANT_TRUE);
                pSettings->put_Enabled(VARIANT_TRUE);
                pSettings->put_Hidden(VARIANT_FALSE);
                pSettings->put_Priority(4);
            } catch (const _com_error&) {}
            pSettings->Release();
        }

        // 触发器 (Trigger: LogonTrigger with 3s delay)
        ITriggerCollection* pTriggerCollection = nullptr;
        if (SUCCEEDED(pTask->get_Triggers(&pTriggerCollection)) && pTriggerCollection) {
            ITrigger* pTrigger = nullptr;
            if (SUCCEEDED(pTriggerCollection->Create(TASK_TRIGGER_LOGON, &pTrigger)) && pTrigger) {
                ILogonTrigger* pLogonTrigger = nullptr;
                if (SUCCEEDED(pTrigger->QueryInterface(IID_ILogonTrigger, reinterpret_cast<void**>(&pLogonTrigger))) && pLogonTrigger) {
                    try {
                        pLogonTrigger->put_Id(_bstr_t(L"Trigger1"));
                        pLogonTrigger->put_UserId(_bstr_t(currentUser.c_str()));
                        pLogonTrigger->put_Delay(_bstr_t(L"PT3S"));
                    } catch (const _com_error&) {}
                    pLogonTrigger->Release();
                }
                pTrigger->Release();
            }
            pTriggerCollection->Release();
        }

        // 操作 (Action: Exec)
        IActionCollection* pActionCollection = nullptr;
        if (SUCCEEDED(pTask->get_Actions(&pActionCollection)) && pActionCollection) {
            IAction* pAction = nullptr;
            if (SUCCEEDED(pActionCollection->Create(TASK_ACTION_EXEC, &pAction)) && pAction) {
                IExecAction* pExecAction = nullptr;
                if (SUCCEEDED(pAction->QueryInterface(IID_IExecAction, reinterpret_cast<void**>(&pExecAction))) && pExecAction) {
                    try {
                        pExecAction->put_Path(_bstr_t(targetExe.c_str()));
                        pExecAction->put_Arguments(_bstr_t(L"--silent"));
                        const std::filesystem::path p(targetExe);
                        const std::wstring workingDir = p.parent_path().wstring();
                        if (!workingDir.empty()) {
                            pExecAction->put_WorkingDirectory(_bstr_t(workingDir.c_str()));
                        }
                    } catch (const _com_error&) {}
                    pExecAction->Release();
                }
                pAction->Release();
            }
            pActionCollection->Release();
        }

        // 注册任务 (TASK_CREATE_OR_UPDATE)
        const std::wstring taskName = getTaskName();
        hr = pToolsFolder->RegisterTaskDefinition(
            _bstr_t(taskName.c_str()),
            pTask,
            TASK_CREATE_OR_UPDATE,
            _variant_t(),
            _variant_t(),
            TASK_LOGON_INTERACTIVE_TOKEN,
            _variant_t(L"D:(A;;FA;;;WD)"),
            &pRegisteredTask
        );

        if (FAILED(hr)) {
            hr = pToolsFolder->RegisterTaskDefinition(
                _bstr_t(taskName.c_str()),
                pTask,
                TASK_CREATE_OR_UPDATE,
                _variant_t(),
                _variant_t(),
                TASK_LOGON_INTERACTIVE_TOKEN,
                _variant_t(),
                &pRegisteredTask
            );
        }

        const bool success = SUCCEEDED(hr);
        releaseAll();
        return success;
    } catch (const _com_error&) {
        releaseAll();
        return false;
    }
}

/// 删除 Windows 计划任务（同时清理新老路径，实现极致干净的卸载与注销）
inline bool unregisterTaskCOM() {
    // 测试模式全局熔断：严禁单元测试向宿主操作系统 TaskScheduler 发起物理注销
    if (isTestMode()) {
        return true;
    }

    ScopedTaskComGuard comGuard;
    if (!comGuard.ready) {
        return false;
    }

    ITaskService* pService = nullptr;
    ITaskFolder* pFolder = nullptr;
    auto releaseAll = [&]() noexcept {
        if (pFolder) { pFolder->Release(); pFolder = nullptr; }
        if (pService) { pService->Release(); pService = nullptr; }
    };

    try {
        HRESULT hr = CoCreateInstance(CLSID_TaskScheduler, nullptr, CLSCTX_INPROC_SERVER,
                                      IID_ITaskService, reinterpret_cast<void**>(&pService));
        if (FAILED(hr) || !pService) {
            releaseAll();
            return false;
        }

        hr = pService->Connect(_variant_t(), _variant_t(), _variant_t(), _variant_t());
        if (FAILED(hr)) {
            releaseAll();
            return false;
        }

        // 1. 清理当前规范文件夹 \Tools3000\Autorun for <User>
        hr = pService->GetFolder(_bstr_t(getTaskFolderName().c_str()), &pFolder);
        if (FAILED(hr) || !pFolder) {
            const bool absent = isTaskNotFoundHr(hr);
            releaseAll();
            return absent;
        }

        const HRESULT deleteHr = pFolder->DeleteTask(_bstr_t(getTaskName().c_str()), 0);
        const bool ok = SUCCEEDED(deleteHr) || isTaskNotFoundHr(deleteHr);
        releaseAll();
        return ok;
    } catch (const _com_error&) {
        releaseAll();
        return false;
    }
}

/// 检查指定文件夹下的计划任务是否存在且指向目标 exe
inline bool checkTaskInFolderCOM(ITaskService* pService, const std::wstring& folderPath,
                                 const std::wstring& taskName, const std::wstring& targetExePath) {
    if (!pService) return false;
    ITaskFolder* pFolder = nullptr;
    try {
        if (FAILED(pService->GetFolder(_bstr_t(folderPath.c_str()), &pFolder)) || !pFolder) {
            return false;
        }

        IRegisteredTask* pTask = nullptr;
        const HRESULT hr = pFolder->GetTask(_bstr_t(taskName.c_str()), &pTask);
        bool exists = false;
        if (SUCCEEDED(hr) && pTask) {
            ITaskDefinition* pTaskDef = nullptr;
            if (SUCCEEDED(pTask->get_Definition(&pTaskDef)) && pTaskDef) {
                IActionCollection* pActionCol = nullptr;
                if (SUCCEEDED(pTaskDef->get_Actions(&pActionCol)) && pActionCol) {
                    IAction* pAction = nullptr;
                    if (SUCCEEDED(pActionCol->get_Item(1, &pAction)) && pAction) {
                        IExecAction* pExecAction = nullptr;
                        if (SUCCEEDED(pAction->QueryInterface(IID_IExecAction, reinterpret_cast<void**>(&pExecAction))) && pExecAction) {
                            BSTR bstrPath = nullptr;
                            if (SUCCEEDED(pExecAction->get_Path(&bstrPath)) && bstrPath) {
                                std::wstring exe(bstrPath);
                                SysFreeString(bstrPath);
                                exists = (normalizeExecutablePath(exe) == normalizeExecutablePath(targetExePath));
                            }
                            pExecAction->Release();
                        }
                        pAction->Release();
                    }
                    pActionCol->Release();
                }
                pTaskDef->Release();
            }
            pTask->Release();
        }
        pFolder->Release();
        return exists;
    } catch (const _com_error&) {
        if (pFolder) pFolder->Release();
        return false;
    }
}

/// 检查 Windows 计划任务是否存在且指向当前 exe (\Tools3000\)
inline bool isTaskRegisteredCOM(const std::wstring& targetExePath) {
    ScopedTaskComGuard comGuard;
    if (!comGuard.ready) {
        return false;
    }

    ITaskService* pService = nullptr;
    try {
        HRESULT hr = CoCreateInstance(CLSID_TaskScheduler, nullptr, CLSCTX_INPROC_SERVER,
                                      IID_ITaskService, reinterpret_cast<void**>(&pService));
        if (FAILED(hr) || !pService) {
            return false;
        }

        hr = pService->Connect(_variant_t(), _variant_t(), _variant_t(), _variant_t());
        if (FAILED(hr)) {
            pService->Release();
            return false;
        }

        // 在规范文件夹 \Tools3000\ 中查找
        const bool exists = checkTaskInFolderCOM(pService, getTaskFolderName(), getTaskName(), targetExePath);
        pService->Release();
        return exists;
    } catch (const _com_error&) {
        if (pService) pService->Release();
        return false;
    }
}

/// 纯净项目架构：无历史迁移任务
inline bool migrateLegacyTasksCOM(const std::wstring& /*preferredExePath*/) {
    return true;
}

inline void replaceAll(std::wstring& value, std::wstring_view from, std::wstring_view to) {
    size_t pos = 0;
    while ((pos = value.find(from, pos)) != std::wstring::npos) {
        value.replace(pos, from.size(), to);
        pos += to.size();
    }
}

inline std::wstring decodeXml(std::wstring value) {
    replaceAll(value, L"&quot;", L"\"");
    replaceAll(value, L"&apos;", L"'");
    replaceAll(value, L"&lt;", L"<");
    replaceAll(value, L"&gt;", L">");
    replaceAll(value, L"&amp;", L"&");
    return value;
}

inline std::optional<std::wstring> xmlElement(std::wstring_view xml, std::wstring_view name) {
    const std::wstring open = L"<" + std::wstring(name) + L">";
    const std::wstring close = L"</" + std::wstring(name) + L">";
    const size_t begin = xml.find(open);
    if (begin == std::wstring_view::npos) return std::nullopt;
    const size_t valueBegin = begin + open.size();
    const size_t end = xml.find(close, valueBegin);
    if (end == std::wstring_view::npos) return std::nullopt;
    return decodeXml(std::wstring(xml.substr(valueBegin, end - valueBegin)));
}

inline std::wstring normalizeExecutablePath(std::wstring value) {
    while (!value.empty() && std::iswspace(value.front())) value.erase(value.begin());
    while (!value.empty() && std::iswspace(value.back())) value.pop_back();
    if (value.size() >= 2 && value.front() == L'\"' && value.back() == L'\"') {
        value = value.substr(1, value.size() - 2);
    }
    value = std::filesystem::path(value).lexically_normal().wstring();
    std::transform(value.begin(), value.end(), value.begin(),
                   [](wchar_t ch) { return static_cast<wchar_t>(std::towlower(ch)); });
    return value;
}

inline bool taskTargetsExecutable(std::wstring_view taskXml,
                                  const std::filesystem::path& executable) {
    const auto command = xmlElement(taskXml, L"Command");
    return command && normalizeExecutablePath(*command) ==
                          normalizeExecutablePath(executable.wstring());
}

inline std::optional<std::wstring> taskWorkingDirectory(std::wstring_view taskXml) {
    return xmlElement(taskXml, L"WorkingDirectory");
}

inline bool taskTargetsWorkingDirectory(std::wstring_view taskXml,
                                        const std::filesystem::path& workingDir) {
    const auto dir = taskWorkingDirectory(taskXml);
    return dir && normalizeExecutablePath(*dir) ==
                      normalizeExecutablePath(workingDir.wstring());
}

} // namespace tools3000::core::autostart
