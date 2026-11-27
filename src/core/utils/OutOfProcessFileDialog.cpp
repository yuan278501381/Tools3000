// ─────────────────────────────────────────────────────────────────────────────
// OutOfProcessFileDialog.cpp — 短命子进程隔离的系统文件与目录选择对话框实现
// ─────────────────────────────────────────────────────────────────────────────

#include "core/utils/OutOfProcessFileDialog.h"
#include "core/logger/Logger.h"
#include "core/utils/WinUtils.h"

#include <nlohmann/json.hpp>
#include <objbase.h>
#include <shobjidl.h>

#include <atomic>
#include <chrono>
#include <fstream>
#include <memory>

namespace tools3000::core {

namespace {

std::atomic<uint64_t> g_dialogJobSeq{1};

struct AnchorCenterPoint {
    bool valid = false;
    int x = 0;
    int y = 0;
};

AnchorCenterPoint resolveMonitorCenterForWindow(HWND ownerHwnd) noexcept {
    HMONITOR hMon = nullptr;
    if (ownerHwnd && IsWindow(ownerHwnd)) {
        hMon = MonitorFromWindow(ownerHwnd, MONITOR_DEFAULTTONEAREST);
    }
    if (!hMon) {
        POINT cursorPt{};
        if (GetCursorPos(&cursorPt)) {
            hMon = MonitorFromPoint(cursorPt, MONITOR_DEFAULTTONEAREST);
        }
    }
    if (!hMon) {
        hMon = MonitorFromPoint(POINT{0, 0}, MONITOR_DEFAULTTOPRIMARY);
    }
    if (!hMon) return {};

    MONITORINFO mi{};
    mi.cbSize = sizeof(mi);
    if (!GetMonitorInfoW(hMon, &mi)) {
        return {};
    }
    AnchorCenterPoint pt{};
    pt.valid = true;
    pt.x = mi.rcWork.left + (mi.rcWork.right - mi.rcWork.left) / 2;
    pt.y = mi.rcWork.top + (mi.rcWork.bottom - mi.rcWork.top) / 2;
    return pt;
}

void bringWorkerDialogToForeground(DWORD workerPid) noexcept {
    if (workerPid == 0) return;
    struct EnumCtx {
        DWORD pid = 0;
        HWND found = nullptr;
    } ctx{workerPid, nullptr};
    EnumWindows([](HWND hwnd, LPARAM lp) -> BOOL {
        auto* c = reinterpret_cast<EnumCtx*>(lp);
        DWORD pid = 0;
        GetWindowThreadProcessId(hwnd, &pid);
        if (pid != c->pid) return TRUE;
        if (!IsWindowVisible(hwnd)) return TRUE;
        if (GetWindow(hwnd, GW_OWNER) != nullptr || (GetWindowLongPtrW(hwnd, GWL_STYLE) & WS_CAPTION) != 0) {
            c->found = hwnd;
            return FALSE;
        }
        if (!c->found) {
            c->found = hwnd;
        }
        return TRUE;
    }, reinterpret_cast<LPARAM>(&ctx));

    if (ctx.found && IsWindow(ctx.found)) {
        if (IsIconic(ctx.found)) {
            ShowWindow(ctx.found, SW_RESTORE);
        }
        SetForegroundWindow(ctx.found);
    }
}

nlohmann::json serializeRequest(const FileDialogRequest& req, const AnchorCenterPoint& anchor) {
    nlohmann::json filtersJson = nlohmann::json::array();
    for (const auto& f : req.filters) {
        filtersJson.push_back({
            {"label", WinUtils::wstringToUtf8(f.label)},
            {"pattern", WinUtils::wstringToUtf8(f.pattern)},
        });
    }
    return {
        {"mode", static_cast<int>(req.mode)},
        // 严禁跨进程将宿主 HWND 传给子进程 IFileDialog::Show(ownerHwnd)：
        // 跨进程模态拥有关系会触发 AttachThreadInput 死锁与全屏 TOPMOST 覆盖层遮挡对话框。
        // 子进程改用同进程同线程隐形锚点定位在宿主所在显示器中心，完全杜绝跨进程互锁。
        {"anchorValid", anchor.valid},
        {"anchorX", anchor.x},
        {"anchorY", anchor.y},
        {"title", WinUtils::wstringToUtf8(req.title)},
        {"defaultExtension", WinUtils::wstringToUtf8(req.defaultExtension)},
        {"defaultFileName", WinUtils::wstringToUtf8(req.defaultFileName)},
        {"initialDirectory", WinUtils::pathToUtf8(req.initialDirectory)},
        {"filters", std::move(filtersJson)},
        {"initialFilterIndex", req.initialFilterIndex},
        {"dontAddToRecent", req.dontAddToRecent},
        {"autoSelectPathForTest", WinUtils::pathToUtf8(req.autoSelectPathForTest)},
    };
}

bool deserializeRequest(const nlohmann::json& j, FileDialogRequest& out, AnchorCenterPoint& outAnchor) {
    if (!j.is_object()) return false;
    const int modeInt = j.value("mode", 0);
    if (modeInt == static_cast<int>(FileDialogMode::SaveFile)) {
        out.mode = FileDialogMode::SaveFile;
    } else if (modeInt == static_cast<int>(FileDialogMode::PickFolder)) {
        out.mode = FileDialogMode::PickFolder;
    } else {
        out.mode = FileDialogMode::OpenFile;
    }
    out.ownerHwnd = nullptr;
    outAnchor.valid = j.value("anchorValid", false);
    outAnchor.x = j.value("anchorX", 0);
    outAnchor.y = j.value("anchorY", 0);
    out.title = WinUtils::utf8ToWstring(j.value("title", ""));
    out.defaultExtension = WinUtils::utf8ToWstring(j.value("defaultExtension", ""));
    out.defaultFileName = WinUtils::utf8ToWstring(j.value("defaultFileName", ""));
    const std::string initDirUtf8 = j.value("initialDirectory", "");
    if (!initDirUtf8.empty()) {
        out.initialDirectory = WinUtils::utf8ToPath(initDirUtf8);
    }
    out.initialFilterIndex = j.value("initialFilterIndex", 1u);
    out.dontAddToRecent = j.value("dontAddToRecent", true);
    const std::string autoTestUtf8 = j.value("autoSelectPathForTest", "");
    if (!autoTestUtf8.empty()) {
        out.autoSelectPathForTest = WinUtils::utf8ToPath(autoTestUtf8);
    }
    out.filters.clear();
    if (j.contains("filters") && j["filters"].is_array()) {
        for (const auto& item : j["filters"]) {
            if (!item.is_object()) continue;
            FileDialogFilterSpec spec{};
            spec.label = WinUtils::utf8ToWstring(item.value("label", ""));
            spec.pattern = WinUtils::utf8ToWstring(item.value("pattern", ""));
            if (!spec.label.empty() && !spec.pattern.empty()) {
                out.filters.push_back(std::move(spec));
            }
        }
    }
    return true;
}

class ScopedInProcessAnchorWindow final {
public:
    explicit ScopedInProcessAnchorWindow(const AnchorCenterPoint& anchor) noexcept {
        const AnchorCenterPoint pt = anchor.valid ? anchor : resolveMonitorCenterForWindow(nullptr);
        const int x = pt.valid ? pt.x : CW_USEDEFAULT;
        const int y = pt.valid ? pt.y : CW_USEDEFAULT;
        m_hwnd = CreateWindowExW(
            WS_EX_TOOLWINDOW,
            L"STATIC",
            L"",
            WS_POPUP,
            x, y, 1, 1,
            nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
        if (m_hwnd) {
            SetForegroundWindow(m_hwnd);
        }
    }

    ~ScopedInProcessAnchorWindow() noexcept {
        if (m_hwnd && IsWindow(m_hwnd)) {
            DestroyWindow(m_hwnd);
        }
    }

    ScopedInProcessAnchorWindow(const ScopedInProcessAnchorWindow&) = delete;
    ScopedInProcessAnchorWindow& operator=(const ScopedInProcessAnchorWindow&) = delete;

    HWND hwnd() const noexcept { return m_hwnd; }

private:
    HWND m_hwnd = nullptr;
};

void waitForWorkerPumpingModalMessages(HANDLE hProcess, DWORD workerPid, HWND modalOwner) noexcept {
    // 若在拥有窗口的 UI 线程等待隔离文件对话框子进程：
    //   1. 临时禁用父窗防重入，并若父窗处于 WS_EX_TOPMOST（如全屏截图覆盖层 / 置顶贴图）则临时降级为 NOTOPMOST，
    //      确保系统文件选择框 100% 浮于最前端可见且不被全屏覆盖层死死遮住；
    //   2. 必须泵送 QS_ALLINPUT（而不仅是 QS_SENDMESSAGE | QS_PAINT），否则任意鼠标点击或队列消息超过 5 秒
    //      不被取走就会触发 Windows Ghost Window（“应用程序没有响应”）；
    //   3. 用户点击已禁用的父窗时自动将焦点拉回子进程文件对话框，还原原生模态体验。
    const DWORD currentTid = GetCurrentThreadId();
    const bool ownsModalWindow =
        modalOwner && IsWindow(modalOwner) && GetWindowThreadProcessId(modalOwner, nullptr) == currentTid;
    const BOOL wasEnabled = ownsModalWindow ? IsWindowEnabled(modalOwner) : FALSE;
    const bool wasTopMost = ownsModalWindow &&
        ((GetWindowLongPtrW(modalOwner, GWL_EXSTYLE) & WS_EX_TOPMOST) != 0);

    if (ownsModalWindow) {
        if (wasEnabled) {
            EnableWindow(modalOwner, FALSE);
        }
        if (wasTopMost) {
            SetWindowPos(modalOwner, HWND_NOTOPMOST, 0, 0, 0, 0,
                         SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_NOOWNERZORDER);
        }
    }

    bool repostQuit = false;
    WPARAM quitCode = 0;

    for (;;) {
        const DWORD waitRes = MsgWaitForMultipleObjectsEx(
            1, &hProcess, 100, QS_ALLINPUT, MWMO_INPUTAVAILABLE);
        if (waitRes == WAIT_OBJECT_0 || waitRes == WAIT_FAILED) {
            break;
        }
        MSG msg{};
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
            if (msg.message == WM_QUIT) {
                repostQuit = true;
                quitCode = msg.wParam;
                break;
            }
            if (ownsModalWindow && msg.hwnd == modalOwner &&
                ((msg.message == WM_SETCURSOR &&
                  (HIWORD(msg.lParam) == WM_LBUTTONDOWN || HIWORD(msg.lParam) == WM_RBUTTONDOWN)) ||
                 msg.message == WM_LBUTTONDOWN || msg.message == WM_RBUTTONDOWN ||
                 msg.message == WM_NCLBUTTONDOWN || msg.message == WM_MOUSEACTIVATE)) {
                bringWorkerDialogToForeground(workerPid);
            }
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
        if (repostQuit) {
            break;
        }
        if (WaitForSingleObject(hProcess, 0) == WAIT_OBJECT_0) {
            break;
        }
    }

    if (ownsModalWindow && IsWindow(modalOwner)) {
        if (wasTopMost) {
            SetWindowPos(modalOwner, HWND_TOPMOST, 0, 0, 0, 0,
                         SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_NOOWNERZORDER);
        }
        if (wasEnabled) {
            EnableWindow(modalOwner, TRUE);
        }
        SetForegroundWindow(modalOwner);
    }

    if (repostQuit) {
        PostQuitMessage(static_cast<int>(quitCode));
    }
}

HANDLE createKillOnCloseJobForChild(HANDLE hChildProcess) noexcept {
    HANDLE hJob = CreateJobObjectW(nullptr, nullptr);
    if (!hJob) return nullptr;
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION jeli{};
    jeli.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
    if (!SetInformationJobObject(hJob, JobObjectExtendedLimitInformation, &jeli, sizeof(jeli)) ||
        !AssignProcessToJobObject(hJob, hChildProcess)) {
        CloseHandle(hJob);
        return nullptr;
    }
    return hJob;
}

std::wstring resolveWorkerExecutablePath() {
    std::wstring buf(MAX_PATH, L'\0');
    for (;;) {
        const DWORD len = GetModuleFileNameW(nullptr, buf.data(), static_cast<DWORD>(buf.size()));
        if (len == 0) return {};
        if (len < buf.size() - 1) {
            buf.resize(len);
            break;
        }
        if (buf.size() >= 32768) return {};
        buf.resize(buf.size() * 2);
    }

    const std::filesystem::path selfPath(buf);
    const std::wstring filenameLower = WinUtils::toLower(selfPath.filename().wstring());
    if (filenameLower.size() >= 9 &&
        filenameLower.compare(filenameLower.size() - 9, 9, L"tests.exe") == 0) {
        std::error_code ec;
        const std::filesystem::path siblingHost = selfPath.parent_path() / L"Tools3000.exe";
        if (std::filesystem::exists(siblingHost, ec)) {
            return siblingHost.wstring();
        }
        return {};
    }
    return buf;
}

FileDialogResult executeInProcessWithAnchor(
    const FileDialogRequest& request,
    const AnchorCenterPoint& anchor) {
    FileDialogResult res{};
    res.selectedFilterIndex = request.initialFilterIndex > 0 ? request.initialFilterIndex : 1u;

    const HRESULT comHr = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
    const bool needUninit = SUCCEEDED(comHr);
    struct ComScope {
        bool active;
        ~ComScope() noexcept {
            if (active) CoUninitialize();
        }
    } comGuard{needUninit};
    if (FAILED(comHr) && comHr != RPC_E_CHANGED_MODE) {
        return res;
    }

    if (!request.autoSelectPathForTest.empty()) {
        res.accepted = true;
        res.selectedPath = request.autoSelectPathForTest;
        return res;
    }

    IFileDialog* dialog = nullptr;
    const CLSID clsid = (request.mode == FileDialogMode::SaveFile)
        ? CLSID_FileSaveDialog
        : CLSID_FileOpenDialog;
    HRESULT hr = CoCreateInstance(clsid, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&dialog));
    if (FAILED(hr) || !dialog) {
        return res;
    }

    DWORD fos = 0;
    if (SUCCEEDED(dialog->GetOptions(&fos))) {
        fos |= FOS_FORCEFILESYSTEM | FOS_NOCHANGEDIR;
        if (request.dontAddToRecent) {
            fos |= FOS_DONTADDTORECENT;
        }
        if (request.mode == FileDialogMode::PickFolder) {
            fos |= FOS_PICKFOLDERS | FOS_PATHMUSTEXIST;
        } else if (request.mode == FileDialogMode::SaveFile) {
            fos |= FOS_PATHMUSTEXIST | FOS_OVERWRITEPROMPT;
        } else {
            fos |= FOS_PATHMUSTEXIST | FOS_FILEMUSTEXIST;
        }
        dialog->SetOptions(fos);
    }

    if (!request.title.empty()) {
        dialog->SetTitle(request.title.c_str());
    }
    if (!request.defaultExtension.empty()) {
        dialog->SetDefaultExtension(request.defaultExtension.c_str());
    }
    if (!request.defaultFileName.empty()) {
        dialog->SetFileName(request.defaultFileName.c_str());
    }
    if (!request.initialDirectory.empty()) {
        std::error_code ec;
        if (std::filesystem::is_directory(request.initialDirectory, ec)) {
            IShellItem* folderItem = nullptr;
            if (SUCCEEDED(SHCreateItemFromParsingName(
                    request.initialDirectory.c_str(), nullptr, IID_PPV_ARGS(&folderItem))) &&
                folderItem) {
                dialog->SetFolder(folderItem);
                folderItem->Release();
            }
        }
    }

    if (request.mode != FileDialogMode::PickFolder && !request.filters.empty()) {
        std::vector<COMDLG_FILTERSPEC> rawFilters;
        rawFilters.reserve(request.filters.size());
        for (const auto& f : request.filters) {
            rawFilters.push_back(COMDLG_FILTERSPEC{f.label.c_str(), f.pattern.c_str()});
        }
        dialog->SetFileTypes(static_cast<UINT>(rawFilters.size()), rawFilters.data());
        const UINT clampedIdx = (request.initialFilterIndex >= 1u &&
                                 request.initialFilterIndex <= rawFilters.size())
            ? static_cast<UINT>(request.initialFilterIndex)
            : 1u;
        dialog->SetFileTypeIndex(clampedIdx);
    }

    // 仅允许绑定同进程同线程的合法父窗；跨进程场景一律改用子进程内部隐形锚点定位到目标显示器中心，
    // 100% 消除跨进程 AttachThreadInput 死锁与全屏覆盖层遮挡。
    HWND safeOwner = nullptr;
    if (request.ownerHwnd && IsWindow(request.ownerHwnd)) {
        DWORD ownerPid = 0;
        GetWindowThreadProcessId(request.ownerHwnd, &ownerPid);
        if (ownerPid == GetCurrentProcessId()) {
            safeOwner = request.ownerHwnd;
        }
    }
    std::unique_ptr<ScopedInProcessAnchorWindow> anchorWin;
    if (!safeOwner) {
        AllowSetForegroundWindow(GetCurrentProcessId());
        anchorWin = std::make_unique<ScopedInProcessAnchorWindow>(anchor);
        safeOwner = anchorWin->hwnd();
    }
    const HRESULT showHr = dialog->Show(safeOwner);
    if (showHr == HRESULT_FROM_WIN32(ERROR_CANCELLED)) {
        res.cancelled = true;
        dialog->Release();
        return res;
    }
    if (SUCCEEDED(showHr)) {
        UINT chosenTypeIdx = 1;
        if (SUCCEEDED(dialog->GetFileTypeIndex(&chosenTypeIdx)) && chosenTypeIdx >= 1u) {
            res.selectedFilterIndex = chosenTypeIdx;
        }
        IShellItem* item = nullptr;
        if (SUCCEEDED(dialog->GetResult(&item)) && item) {
            PWSTR rawPath = nullptr;
            if (SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &rawPath)) && rawPath) {
                res.selectedPath = std::filesystem::path(rawPath);
                res.accepted = !res.selectedPath.empty();
                CoTaskMemFree(rawPath);
            }
            item->Release();
        }
    }
    dialog->Release();
    return res;
}

} // namespace

FileDialogResult OutOfProcessFileDialog::executeInProcess(const FileDialogRequest& request) {
    return executeInProcessWithAnchor(request, resolveMonitorCenterForWindow(request.ownerHwnd));
}

FileDialogResult OutOfProcessFileDialog::show(const FileDialogRequest& request) {
    const std::wstring exePath = resolveWorkerExecutablePath();
    if (exePath.empty()) {
        return executeInProcess(request);
    }

    std::error_code ec;
    const std::filesystem::path tempDir = WinUtils::getAppDataDirectory() / L"temp";
    std::filesystem::create_directories(tempDir, ec);
    const uint64_t seq = g_dialogJobSeq.fetch_add(1, std::memory_order_relaxed);
    const std::wstring stem =
        L"fdlg_" + std::to_wstring(GetCurrentProcessId()) + L"_" + std::to_wstring(seq);
    const std::filesystem::path jobPath = tempDir / (stem + L"_in.json");
    const std::filesystem::path outPath = tempDir / (stem + L"_out.json");

    struct ScopedTempCleanup {
        std::filesystem::path a;
        std::filesystem::path b;
        ~ScopedTempCleanup() noexcept {
            std::error_code ignoreEc;
            std::filesystem::remove(a, ignoreEc);
            std::filesystem::remove(b, ignoreEc);
        }
    } cleanup{jobPath, outPath};

    const AnchorCenterPoint anchor = resolveMonitorCenterForWindow(request.ownerHwnd);
    nlohmann::json jobDoc = {
        {"request", serializeRequest(request, anchor)},
        {"outputPath", WinUtils::pathToUtf8(outPath)},
    };
    const std::string jobBytes = jobDoc.dump(-1, ' ', false, nlohmann::json::error_handler_t::replace);
    if (!WinUtils::atomicWriteFileWithFlush(jobPath.wstring(), jobBytes)) {
        return executeInProcess(request);
    }

    std::wstring cmdLine = L"\"" + exePath + L"\" --worker=file-dialog \"--job=" + jobPath.wstring() + L"\"";
    STARTUPINFOW si{};
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi{};
    const std::wstring workDir = std::filesystem::path(exePath).parent_path().wstring();
    if (!CreateProcessW(
            exePath.c_str(),
            cmdLine.data(),
            nullptr,
            nullptr,
            FALSE,
            0,
            nullptr,
            workDir.empty() ? nullptr : workDir.c_str(),
            &si,
            &pi)) {
        LOG_WARN("OutOfProcessFileDialog: CreateProcessW failed (err={}), falling back in-process", GetLastError());
        return executeInProcess(request);
    }

    HANDLE hJob = createKillOnCloseJobForChild(pi.hProcess);
    AllowSetForegroundWindow(pi.dwProcessId);
    CloseHandle(pi.hThread);
    waitForWorkerPumpingModalMessages(pi.hProcess, pi.dwProcessId, request.ownerHwnd);
    DWORD exitCode = 1;
    GetExitCodeProcess(pi.hProcess, &exitCode);
    CloseHandle(pi.hProcess);
    if (hJob) {
        CloseHandle(hJob);
    }

    if (exitCode != 0 || !std::filesystem::exists(outPath, ec)) {
        LOG_WARN("OutOfProcessFileDialog: worker exited abnormally (code={}), falling back in-process", exitCode);
        return executeInProcess(request);
    }

    try {
        std::ifstream ifs(outPath, std::ios::binary);
        if (!ifs) return {};
        const nlohmann::json outJson = nlohmann::json::parse(ifs);
        FileDialogResult res{};
        res.ranOutOfProcess = true;
        res.accepted = outJson.value("accepted", false);
        res.cancelled = outJson.value("cancelled", false);
        res.selectedFilterIndex = outJson.value("selectedFilterIndex", 1u);
        const std::string pathUtf8 = outJson.value("selectedPath", "");
        if (!pathUtf8.empty()) {
            res.selectedPath = WinUtils::utf8ToPath(pathUtf8);
        }
        return res;
    } catch (const std::exception& e) {
        LOG_WARN("OutOfProcessFileDialog: failed to parse worker output: {}", e.what());
        return {};
    }
}

int OutOfProcessFileDialog::runWorkerFromJobFile(const std::filesystem::path& jobFilePath) {
    std::error_code ec;
    if (jobFilePath.empty() || !std::filesystem::exists(jobFilePath, ec)) {
        return 2;
    }

    nlohmann::json jobDoc;
    try {
        std::ifstream ifs(jobFilePath, std::ios::binary);
        if (!ifs) return 3;
        jobDoc = nlohmann::json::parse(ifs);
    } catch (...) {
        return 4;
    }

    const std::string outPathUtf8 = jobDoc.value("outputPath", "");
    if (outPathUtf8.empty() || !jobDoc.contains("request")) {
        return 5;
    }

    FileDialogRequest req{};
    AnchorCenterPoint anchor{};
    if (!deserializeRequest(jobDoc["request"], req, anchor)) {
        return 6;
    }

    const FileDialogResult res = executeInProcessWithAnchor(req, anchor);
    const nlohmann::json outDoc = {
        {"accepted", res.accepted},
        {"cancelled", res.cancelled},
        {"selectedFilterIndex", res.selectedFilterIndex},
        {"selectedPath", WinUtils::pathToUtf8(res.selectedPath)},
    };
    const std::filesystem::path outPath = WinUtils::utf8ToPath(outPathUtf8);
    const std::string payload = outDoc.dump(-1, ' ', false, nlohmann::json::error_handler_t::replace);
    return WinUtils::atomicWriteFileWithFlush(outPath.wstring(), payload) ? 0 : 7;
}

} // namespace tools3000::core
