/**
 * Cross-process integration gate for DialogNavigator.
 *
 * Launches child-owned modern IFileDialog (folder / open / save) and legacy
 * OFN_EXPLORER (GetOpenFileNameW + hook) dialogs, verifies architecture/mode
 * classification, millisecond folder navigation, Save As filename protection,
 * OpenFile search-result filename fill without unintended commit, and clean
 * window lifecycle teardown.
 */

#include "dialog/DialogNavigator.h"
#include "core/logger/Logger.h"
#include "core/utils/WinUtils.h"

#include <windows.h>
#include <commdlg.h>
#include <shobjidl.h>
#include <uiautomation.h>
#include <wrl/client.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <thread>

#pragma comment(lib, "Comdlg32.lib")

using Microsoft::WRL::ComPtr;

namespace {

std::wstring quote(const std::wstring& value) {
    return L"\"" + value + L"\"";
}

HWND findDialogForProcess(DWORD processId) {
    struct Context { DWORD pid; HWND hwnd; } context{processId, nullptr};
    EnumWindows([](HWND hwnd, LPARAM param) -> BOOL {
        auto* context = reinterpret_cast<Context*>(param);
        DWORD pid = 0;
        GetWindowThreadProcessId(hwnd, &pid);
        if (pid != context->pid) return TRUE;
        wchar_t className[64]{};
        GetClassNameW(hwnd, className, static_cast<int>(std::size(className)));
        if (wcscmp(className, L"#32770") == 0 && IsWindowVisible(hwnd)) {
            context->hwnd = hwnd;
            return FALSE;
        }
        return TRUE;
    }, reinterpret_cast<LPARAM>(&context));
    return context.hwnd;
}

void writeFolderFile(const std::wstring& folderW, const std::filesystem::path& resultPath) {
    if (folderW.empty()) return;
    std::ofstream output(resultPath, std::ios::binary | std::ios::trunc);
    output << tools3000::core::WinUtils::wstringToUtf8(folderW);
}

void writeCurrentFolder(IFileDialog* dialog, const std::filesystem::path& resultPath) {
    if (!dialog) return;
    ComPtr<IShellItem> item;
    if (FAILED(dialog->GetFolder(item.GetAddressOf())) || !item) return;
    PWSTR rawPath = nullptr;
    if (FAILED(item->GetDisplayName(SIGDN_FILESYSPATH, &rawPath)) || !rawPath) return;
    writeFolderFile(rawPath, resultPath);
    CoTaskMemFree(rawPath);
}

class FolderChangeSink final : public IFileDialogEvents {
public:
    explicit FolderChangeSink(std::filesystem::path resultPath)
        : resultPath_(std::move(resultPath)) {}

    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** object) override {
        if (!object) return E_POINTER;
        *object = nullptr;
        if (riid == IID_IUnknown || riid == IID_IFileDialogEvents) {
            *object = static_cast<IFileDialogEvents*>(this);
            AddRef();
            return S_OK;
        }
        return E_NOINTERFACE;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return ++refs_; }
    ULONG STDMETHODCALLTYPE Release() override {
        const ULONG refs = --refs_;
        if (!refs) delete this;
        return refs;
    }
    HRESULT STDMETHODCALLTYPE OnFileOk(IFileDialog*) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE OnFolderChanging(IFileDialog*, IShellItem*) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE OnFolderChange(IFileDialog* dialog) override {
        writeCurrentFolder(dialog, resultPath_);
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE OnSelectionChange(IFileDialog*) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE OnShareViolation(IFileDialog*, IShellItem*,
                                                FDE_SHAREVIOLATION_RESPONSE* response) override {
        if (response) *response = FDESVR_DEFAULT;
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE OnTypeChange(IFileDialog*) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE OnOverwrite(IFileDialog*, IShellItem*,
                                          FDE_OVERWRITE_RESPONSE* response) override {
        if (response) *response = FDEOR_DEFAULT;
        return S_OK;
    }

private:
    std::atomic<ULONG> refs_{1};
    std::filesystem::path resultPath_;
};

std::filesystem::path* g_legacyResultPath = nullptr;

UINT_PTR CALLBACK LegacyOfnHookProc(HWND hdlg, UINT uiMsg, WPARAM, LPARAM lParam) {
    if (uiMsg == WM_NOTIFY && lParam && g_legacyResultPath) {
        const auto* nm = reinterpret_cast<const OFNOTIFYW*>(lParam);
        if (nm->hdr.code == CDN_FOLDERCHANGE || nm->hdr.code == CDN_INITDONE) {
            HWND dlgRoot = GetParent(hdlg);
            wchar_t buf[MAX_PATH * 4]{};
            if (dlgRoot && CommDlg_OpenSave_GetFolderPathW(dlgRoot, buf, static_cast<int>(std::size(buf))) > 0) {
                writeFolderFile(buf, *g_legacyResultPath);
            }
        }
    }
    return 0;
}

int runLegacyChild(const std::filesystem::path& resultPath) {
    std::filesystem::path pathStorage = resultPath;
    g_legacyResultPath = &pathStorage;

    wchar_t fileBuf[MAX_PATH * 2] = L"legacy-draft.txt";
    OPENFILENAMEW ofn{};
    ofn.lStructSize = sizeof(ofn);
    ofn.lpstrFilter = L"All Files (*.*)\0*.*\0";
    ofn.lpstrFile = fileBuf;
    ofn.nMaxFile = static_cast<DWORD>(std::size(fileBuf));
    ofn.lpstrTitle = L"Tools3000 Legacy OFN_EXPLORER Probe";
    ofn.Flags = OFN_EXPLORER | OFN_ENABLEHOOK | OFN_NOCHANGEDIR;
    ofn.lpfnHook = LegacyOfnHookProc;
    (void)GetOpenFileNameW(&ofn);

    g_legacyResultPath = nullptr;
    std::string folder;
    std::ifstream input(resultPath, std::ios::binary);
    std::getline(input, folder);
    return folder.empty() ? 14 : 0;
}

int runChild(const std::filesystem::path& resultPath, const std::wstring& mode) {
    if (mode == L"legacy_open") {
        return runLegacyChild(resultPath);
    }

    if (FAILED(CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED))) return 10;

    const CLSID& dialogClass = mode == L"save" ? CLSID_FileSaveDialog : CLSID_FileOpenDialog;
    ComPtr<IFileDialog> dialog;
    HRESULT hr = CoCreateInstance(dialogClass, nullptr, CLSCTX_INPROC_SERVER,
                                  IID_PPV_ARGS(dialog.GetAddressOf()));
    if (FAILED(hr) || !dialog) {
        CoUninitialize();
        return 11;
    }

    DWORD options = 0;
    dialog->GetOptions(&options);
    options |= FOS_FORCEFILESYSTEM;
    if (mode == L"folder") options |= FOS_PICKFOLDERS;
    dialog->SetOptions(options);
    if (mode == L"save") dialog->SetFileName(L"draft.txt");
    dialog->SetTitle(L"Tools3000 DialogNavigator Integration Probe");
    auto* sink = new FolderChangeSink(resultPath);
    DWORD eventCookie = 0;
    dialog->Advise(sink, &eventCookie);
    sink->Release();
    dialog->Show(nullptr);
    dialog->Unadvise(eventCookie);

    std::string folder;
    std::ifstream input(resultPath, std::ios::binary);
    std::getline(input, folder);
    CoUninitialize();
    return folder.empty() ? 12 : 0;
}

bool pathEquals(const std::filesystem::path& left, const std::filesystem::path& right) {
    auto norm = [](std::wstring s) {
        while (s.size() > 3 && s.back() == L'\\') s.pop_back();
        return s;
    };
    const std::wstring a = norm(left.lexically_normal().wstring());
    const std::wstring b = norm(right.lexically_normal().wstring());
    return _wcsicmp(a.c_str(), b.c_str()) == 0;
}

std::wstring readControlText(HWND dialogHwnd, int controlId, bool useUiaFallback) {
    HWND targetEdit = tools3000::dialog::DialogNavigator::findPathEditControl(dialogHwnd);
    if (!targetEdit) {
        struct Search { int id; HWND hwnd; } search{controlId, nullptr};
        EnumChildWindows(dialogHwnd, [](HWND child, LPARAM parameter) -> BOOL {
            auto* search = reinterpret_cast<Search*>(parameter);
            wchar_t className[32]{};
            GetClassNameW(child, className, static_cast<int>(std::size(className)));
            if (wcscmp(className, L"Edit") == 0 && GetDlgCtrlID(child) == search->id) {
                search->hwnd = child;
                return FALSE;
            }
            return TRUE;
        }, reinterpret_cast<LPARAM>(&search));
        targetEdit = search.hwnd;
    }

    if (targetEdit) {
        DWORD_PTR rawLen = 0;
        if (SendMessageTimeoutW(targetEdit, WM_GETTEXTLENGTH, 0, 0,
                                SMTO_ABORTIFHUNG | SMTO_BLOCK, 300, &rawLen) && rawLen > 0) {
            std::wstring text(static_cast<size_t>(rawLen) + 2, L'\0');
            DWORD_PTR copied = 0;
            if (SendMessageTimeoutW(targetEdit, WM_GETTEXT, static_cast<WPARAM>(text.size()),
                                    reinterpret_cast<LPARAM>(text.data()),
                                    SMTO_ABORTIFHUNG | SMTO_BLOCK, 300, &copied) && copied > 0) {
                text.resize(static_cast<size_t>(copied));
                return text;
            }
        }
    }

    if (!useUiaFallback) return {};
    ComPtr<IUIAutomation> uia;
    if (FAILED(CoCreateInstance(CLSID_CUIAutomation, nullptr, CLSCTX_INPROC_SERVER,
                                IID_PPV_ARGS(uia.GetAddressOf()))) || !uia) return {};
    ComPtr<IUIAutomationElement> root;
    if (FAILED(uia->ElementFromHandle(dialogHwnd, root.GetAddressOf())) || !root) return {};
    VARIANT editType{};
    editType.vt = VT_I4;
    editType.lVal = UIA_EditControlTypeId;
    ComPtr<IUIAutomationCondition> condition;
    ComPtr<IUIAutomationElementArray> edits;
    uia->CreatePropertyCondition(UIA_ControlTypePropertyId, editType, condition.GetAddressOf());
    if (!condition || FAILED(root->FindAll(TreeScope_Descendants, condition.Get(),
                                           edits.GetAddressOf())) || !edits) return {};
    int count = 0;
    edits->get_Length(&count);
    LONG lowestTop = LONG_MIN;
    std::wstring lowestValue;
    for (int index = 0; index < count; ++index) {
        ComPtr<IUIAutomationElement> edit;
        edits->GetElement(index, edit.GetAddressOf());
        if (!edit) continue;
        RECT rect{};
        ComPtr<IUIAutomationValuePattern> pattern;
        if (FAILED(edit->get_CurrentBoundingRectangle(&rect)) || rect.top < lowestTop ||
            FAILED(edit->GetCurrentPatternAs(UIA_ValuePatternId, IID_IUIAutomationValuePattern,
                   reinterpret_cast<void**>(pattern.GetAddressOf()))) || !pattern) continue;
        BSTR raw = nullptr;
        if (SUCCEEDED(pattern->get_CurrentValue(&raw)) && raw) {
            lowestTop = rect.top;
            lowestValue.assign(raw);
        }
        SysFreeString(raw);
    }
    return lowestValue;
}

bool writeBottomSelectionText(HWND dialogHwnd, const std::wstring& value) {
    HWND edit = tools3000::dialog::DialogNavigator::findPathEditControl(dialogHwnd);
    if (!edit) return false;
    DWORD_PTR ignored = 0;
    return SendMessageTimeoutW(
               edit, WM_SETTEXT, 0, reinterpret_cast<LPARAM>(value.c_str()),
               SMTO_ABORTIFHUNG | SMTO_BLOCK, 500, &ignored) != 0;
}

bool selectShellItemByName(HWND dialogHwnd, const std::wstring& itemName) {
    ComPtr<IUIAutomation> uia;
    if (FAILED(CoCreateInstance(CLSID_CUIAutomation, nullptr, CLSCTX_INPROC_SERVER,
                                IID_PPV_ARGS(uia.GetAddressOf()))) || !uia) return false;
    ComPtr<IUIAutomationElement> root;
    if (FAILED(uia->ElementFromHandle(dialogHwnd, root.GetAddressOf())) || !root) return false;

    VARIANT listItemType{};
    listItemType.vt = VT_I4;
    listItemType.lVal = UIA_ListItemControlTypeId;
    VARIANT dataItemType{};
    dataItemType.vt = VT_I4;
    dataItemType.lVal = UIA_DataItemControlTypeId;
    ComPtr<IUIAutomationCondition> listItemCondition;
    ComPtr<IUIAutomationCondition> dataItemCondition;
    ComPtr<IUIAutomationCondition> shellItemTypeCondition;
    if (FAILED(uia->CreatePropertyCondition(
            UIA_ControlTypePropertyId, listItemType,
            listItemCondition.GetAddressOf())) || !listItemCondition ||
        FAILED(uia->CreatePropertyCondition(
            UIA_ControlTypePropertyId, dataItemType,
            dataItemCondition.GetAddressOf())) || !dataItemCondition ||
        FAILED(uia->CreateOrCondition(
            listItemCondition.Get(), dataItemCondition.Get(),
            shellItemTypeCondition.GetAddressOf())) || !shellItemTypeCondition) {
        return false;
    }

    for (int retry = 0; retry < 20; ++retry) {
        ComPtr<IUIAutomationElementArray> items;
        if (FAILED(root->FindAll(TreeScope_Descendants, shellItemTypeCondition.Get(),
                                 items.GetAddressOf())) || !items) {
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
            continue;
        }
        int count = 0;
        items->get_Length(&count);
        for (int index = 0; index < count; ++index) {
            ComPtr<IUIAutomationElement> item;
            if (FAILED(items->GetElement(index, item.GetAddressOf())) || !item) continue;
            BSTR rawName = nullptr;
            if (FAILED(item->get_CurrentName(&rawName)) || !rawName) continue;
            const std::wstring accessibleName(rawName);
            SysFreeString(rawName);
            if (accessibleName != itemName && !accessibleName.starts_with(itemName)) continue;

            ComPtr<IUIAutomationSelectionItemPattern> selection;
            if (SUCCEEDED(item->GetCurrentPatternAs(
                    UIA_SelectionItemPatternId, IID_IUIAutomationSelectionItemPattern,
                    reinterpret_cast<void**>(selection.GetAddressOf()))) && selection &&
                SUCCEEDED(selection->Select())) {
                return true;
            }
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    return false;
}

}  // namespace

int wmain(int argc, wchar_t** argv) {
    if (argc == 4 && wcscmp(argv[1], L"--dialog-child") == 0) {
        return runChild(argv[2], argv[3]);
    }

    wchar_t executable[MAX_PATH]{};
    if (!GetModuleFileNameW(nullptr, executable, static_cast<DWORD>(std::size(executable)))) return 20;

    const auto probeRoot = std::filesystem::temp_directory_path() /
                           (L"tools3000-dialog-probe-" + std::to_wstring(GetCurrentProcessId()));
    const auto target = probeRoot / L"101方案_控制";
    const auto selectedChild = target / L"平安下达生产订单方案V9306";
    const auto searchTargetDir = probeRoot / L"搜索直达目录";
    const auto searchTargetFile = searchTargetDir / L"sample-note.txt";

    std::error_code ec;
    std::filesystem::remove_all(probeRoot, ec);
    if (!std::filesystem::create_directories(selectedChild, ec)) return 21;
    if (!std::filesystem::create_directories(searchTargetDir, ec)) return 21;
    {
        std::ofstream f(searchTargetFile, std::ios::binary | std::ios::trunc);
        f << "Tools3000 integration sample";
    }

    using tools3000::dialog::DialogArchitecture;
    using tools3000::dialog::DialogMode;
    using tools3000::dialog::DialogNavigator;

    bool allSucceeded = true;
    for (const std::wstring mode : {L"folder", L"open", L"save", L"legacy_open"}) {
        const auto result = probeRoot / (mode + L"-result.txt");
        std::wstring command = quote(executable) + L" --dialog-child " +
                               quote(result.native()) + L" " + quote(mode);
        STARTUPINFOW startup{sizeof(startup)};
        PROCESS_INFORMATION process{};
        if (!CreateProcessW(nullptr, command.data(), nullptr, nullptr, FALSE, 0,
                            nullptr, nullptr, &startup, &process)) {
            allSucceeded = false;
            continue;
        }

        HWND dialogHwnd = nullptr;
        for (int retry = 0; retry < 100 && !dialogHwnd; ++retry) {
            dialogHwnd = findDialogForProcess(process.dwProcessId);
            if (!dialogHwnd) std::this_thread::sleep_for(std::chrono::milliseconds(50));
        }

        const bool recognized = dialogHwnd && DialogNavigator::isFileDialog(dialogHwnd);
        if (recognized) std::this_thread::sleep_for(std::chrono::milliseconds(200));

        const DialogArchitecture detectedArch = dialogHwnd
            ? DialogNavigator::detectDialogArchitecture(dialogHwnd, false)
            : DialogArchitecture::Unknown;
        const DialogMode detectedMode = dialogHwnd
            ? DialogNavigator::detectDialogMode(dialogHwnd, false)
            : DialogMode::Unknown;

        const bool preserveSaveName = (mode == L"save" || mode == L"legacy_open");
        const std::wstring bottomBefore = dialogHwnd
            ? readControlText(dialogHwnd, 1152, preserveSaveName) : L"";

        bool navigateIssued = false;
        bool remainedOpen = false;
        bool archAndModeOk = false;
        bool searchResultActionOk = true;
        bool selectionCorrect = true;
        std::string currentAfterNavigation;
        std::wstring bottomAfter;
        std::filesystem::path expectedFinalDir = target;

        bool success = recognized;
        if (success) {
            if (mode == L"folder") {
                archAndModeOk = (detectedArch == DialogArchitecture::ModernIFileDialog) &&
                                (detectedMode == DialogMode::PickFolder);
            } else if (mode == L"open") {
                archAndModeOk = (detectedArch == DialogArchitecture::ModernIFileDialog) &&
                                (detectedMode == DialogMode::OpenFile);
            } else if (mode == L"save") {
                archAndModeOk = (detectedArch == DialogArchitecture::ModernIFileDialog) &&
                                (detectedMode == DialogMode::SaveFile);
            } else if (mode == L"legacy_open") {
                archAndModeOk = (detectedArch == DialogArchitecture::LegacyExplorer ||
                                 detectedArch == DialogArchitecture::ClassicOpenFileName);
            }

            SetForegroundWindow(dialogHwnd);
            std::this_thread::sleep_for(std::chrono::milliseconds(80));
            navigateIssued = DialogNavigator::instance().navigateToFolder(
                dialogHwnd, tools3000::core::WinUtils::pathToUtf8(target));
            std::this_thread::sleep_for(std::chrono::milliseconds(220));
            remainedOpen = IsWindow(dialogHwnd) != FALSE;
            currentAfterNavigation = DialogNavigator::getCurrentDialogFolder(dialogHwnd);
            bottomAfter = readControlText(dialogHwnd, 1152, preserveSaveName);

            if (mode == L"folder") {
                bool childSelected = selectShellItemByName(dialogHwnd, L"平安下达生产订单方案V9306");
                if (!childSelected) {
                    childSelected = writeBottomSelectionText(dialogHwnd, L"平安下达生产订单方案V9306");
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(150));
                const std::string selectedPath = DialogNavigator::getSelectedPath(dialogHwnd);
                selectionCorrect = childSelected && !selectedPath.empty() &&
                                   pathEquals(tools3000::core::WinUtils::utf8ToPath(selectedPath), selectedChild);
            } else if (mode == L"save") {
                // 实机验证 Save As 选中文件（即使按住 Shift+Enter）：
                // 强制降级为仅跳转父目录，draft.txt 毫发无损，对话框保持打开绝不自动保存提交！
                const bool safeJumpOk = DialogNavigator::instance().navigateToSearchResult(
                    dialogHwnd,
                    tools3000::core::WinUtils::pathToUtf8(searchTargetFile),
                    /*itemIsDirectory=*/false,
                    DialogMode::SaveFile,
                    /*ctrlPressed=*/false,
                    /*shiftPressed=*/true);
                std::this_thread::sleep_for(std::chrono::milliseconds(180));
                const bool stillOpen = IsWindow(dialogHwnd) != FALSE;
                const std::wstring saveBottomNow = readControlText(dialogHwnd, 1152, true);
                searchResultActionOk = safeJumpOk && stillOpen && (saveBottomNow == L"draft.txt");
                expectedFinalDir = searchTargetDir;
            } else if (mode == L"open") {
                // 实机验证 OpenFile 选中文件（Enter）：
                // 先跳父目录并验证到位，再填入 sample-note.txt，且保持打开不自动关闭！
                const bool fillOk = DialogNavigator::instance().navigateToSearchResult(
                    dialogHwnd,
                    tools3000::core::WinUtils::pathToUtf8(searchTargetFile),
                    /*itemIsDirectory=*/false,
                    DialogMode::OpenFile,
                    /*ctrlPressed=*/false,
                    /*shiftPressed=*/false);
                std::this_thread::sleep_for(std::chrono::milliseconds(180));
                const bool stillOpen = IsWindow(dialogHwnd) != FALSE;
                const std::wstring openBottomNow = readControlText(dialogHwnd, 1152, true);
                searchResultActionOk = fillOk && stillOpen && (openBottomNow == L"sample-note.txt");
                expectedFinalDir = searchTargetDir;
            }

            const bool bottomPreserved = (mode != L"save" || bottomAfter == L"draft.txt") &&
                                         (mode != L"legacy_open" || bottomAfter == bottomBefore);
            const bool modernFolderMatches = (mode == L"legacy_open") ||
                (!currentAfterNavigation.empty() &&
                 pathEquals(tools3000::core::WinUtils::utf8ToPath(currentAfterNavigation), target));

            success = navigateIssued && remainedOpen && archAndModeOk &&
                      selectionCorrect && modernFolderMatches &&
                      bottomPreserved && searchResultActionOk;

            PostMessageW(dialogHwnd, WM_CLOSE, 0, 0);
        }

        const DWORD waitResult = WaitForSingleObject(process.hProcess, 5000);
        DWORD childExit = STILL_ACTIVE;
        GetExitCodeProcess(process.hProcess, &childExit);
        CloseHandle(process.hThread);
        CloseHandle(process.hProcess);

        std::string reportedFolder;
        std::ifstream input(result, std::ios::binary);
        std::getline(input, reportedFolder);
        success = success && waitResult == WAIT_OBJECT_0 && childExit == 0 &&
                  !reportedFolder.empty() &&
                  pathEquals(tools3000::core::WinUtils::utf8ToPath(reportedFolder), expectedFinalDir);
        allSucceeded = allSucceeded && success;

        std::cout << "mode=" << tools3000::core::WinUtils::wstringToUtf8(mode)
                  << " dialog_opened=" << (dialogHwnd != nullptr)
                  << " recognized=" << recognized
                  << " arch=" << static_cast<int>(detectedArch)
                  << " detected_mode=" << static_cast<int>(detectedMode)
                  << " arch_mode_ok=" << archAndModeOk
                  << " navigate_issued=" << navigateIssued
                  << " remained_open=" << remainedOpen
                  << " search_action_ok=" << searchResultActionOk
                  << " bottom_before=" << tools3000::core::WinUtils::wstringToUtf8(bottomBefore)
                  << " bottom_after=" << tools3000::core::WinUtils::wstringToUtf8(bottomAfter)
                  << " reported_folder=" << reportedFolder
                  << " expected_folder=" << tools3000::core::WinUtils::pathToUtf8(expectedFinalDir)
                  << " result=" << (success ? "PASS" : "FAIL") << '\n';
    }

    std::filesystem::remove_all(probeRoot, ec);
    return allSucceeded ? 0 : 23;
}
