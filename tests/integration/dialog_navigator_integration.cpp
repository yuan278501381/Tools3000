/**
 * Cross-process integration gate for DialogNavigator.
 *
 * The parent launches child-owned folder/open/save dialogs, navigates each from
 * another process, verifies the dialog remains open and keeps its bottom input,
 * then uses IFileDialogEvents to report the authoritative current folder.
 */

#include "dialog/DialogNavigator.h"
#include "core/logger/Logger.h"
#include "core/utils/WinUtils.h"

#include <windows.h>
#include <shobjidl.h>
#include <uiautomation.h>
#include <wrl/client.h>

#include <chrono>
#include <atomic>
#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <thread>

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

void writeCurrentFolder(IFileDialog* dialog, const std::filesystem::path& resultPath) {
    if (!dialog) return;
    ComPtr<IShellItem> item;
    if (FAILED(dialog->GetFolder(item.GetAddressOf())) || !item) return;
    PWSTR rawPath = nullptr;
    if (FAILED(item->GetDisplayName(SIGDN_FILESYSPATH, &rawPath)) || !rawPath) return;
    std::ofstream output(resultPath, std::ios::binary | std::ios::trunc);
    output << tools3000::core::WinUtils::wstringToUtf8(rawPath);
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

int runChild(const std::filesystem::path& resultPath, const std::wstring& mode) {
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
    return _wcsicmp(left.lexically_normal().c_str(), right.lexically_normal().c_str()) == 0;
}

std::wstring readControlText(HWND dialogHwnd, int controlId, bool useUiaFallback) {
    struct Search { int id; HWND hwnd; HWND lowestEdit; LONG lowestTop; }
        search{controlId, nullptr, nullptr, LONG_MIN};
    EnumChildWindows(dialogHwnd, [](HWND child, LPARAM parameter) -> BOOL {
        auto* search = reinterpret_cast<Search*>(parameter);
        wchar_t className[32]{};
        GetClassNameW(child, className, static_cast<int>(std::size(className)));
        if (wcscmp(className, L"Edit") != 0) return TRUE;
        RECT rect{};
        if (GetWindowRect(child, &rect) && rect.top > search->lowestTop) {
            search->lowestTop = rect.top;
            search->lowestEdit = child;
        }
        if (GetDlgCtrlID(child) == search->id) {
            search->hwnd = child;
            return FALSE;
        }
        return TRUE;
    }, reinterpret_cast<LPARAM>(&search));
    if (!search.hwnd) search.hwnd = search.lowestEdit;
    if (search.hwnd) {
        const int length = GetWindowTextLengthW(search.hwnd);
        std::wstring text(static_cast<size_t>(std::max(0, length)) + 1, L'\0');
        const int copied = GetWindowTextW(search.hwnd, text.data(), static_cast<int>(text.size()));
        text.resize(static_cast<size_t>(std::max(0, copied)));
        if (!text.empty()) return text;
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
    struct Search { HWND exact; HWND lowestEdit; LONG lowestTop; }
        search{nullptr, nullptr, LONG_MIN};
    EnumChildWindows(dialogHwnd, [](HWND child, LPARAM parameter) -> BOOL {
        auto* search = reinterpret_cast<Search*>(parameter);
        wchar_t className[32]{};
        GetClassNameW(child, className, static_cast<int>(std::size(className)));
        if (wcscmp(className, L"Edit") != 0) return TRUE;
        RECT rect{};
        if (GetWindowRect(child, &rect) && rect.top > search->lowestTop) {
            search->lowestTop = rect.top;
            search->lowestEdit = child;
        }
        if (GetDlgCtrlID(child) == 1152) search->exact = child;
        return TRUE;
    }, reinterpret_cast<LPARAM>(&search));
    const HWND edit = search.exact ? search.exact : search.lowestEdit;
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

} // namespace

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
    std::error_code ec;
    std::filesystem::remove_all(probeRoot, ec);
    if (!std::filesystem::create_directories(selectedChild, ec)) return 21;

    bool allSucceeded = true;
    for (const std::wstring mode : {L"folder", L"open", L"save"}) {
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

        const bool recognized = dialogHwnd && tools3000::dialog::DialogNavigator::isFileDialog(dialogHwnd);
        if (recognized) std::this_thread::sleep_for(std::chrono::milliseconds(200));
        const bool preserveSaveName = mode == L"save";
        const std::wstring bottomBefore = dialogHwnd
            ? readControlText(dialogHwnd, 1152, preserveSaveName) : L"";
        bool navigateIssued = false;
        bool remainedOpen = false;
        bool selectionEmpty = false;
        bool childSelectionIssued = false;
        bool childSelectionMatches = false;
        std::string selectedAfterNavigation;
        std::string currentAfterNavigation;
        std::wstring bottomAfter;
        bool success = recognized;
        if (success) {
            SetForegroundWindow(dialogHwnd);
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
            navigateIssued = tools3000::dialog::DialogNavigator::instance().navigateToFolder(
                dialogHwnd, tools3000::core::WinUtils::wstringToUtf8(target.native()));
            std::this_thread::sleep_for(std::chrono::milliseconds(350));
            remainedOpen = IsWindow(dialogHwnd);
            currentAfterNavigation = tools3000::dialog::DialogNavigator::getCurrentDialogFolder(dialogHwnd);
            if (mode == L"folder") {
                childSelectionIssued = selectShellItemByName(
                    dialogHwnd, L"平安下达生产订单方案V9306");
                if (!childSelectionIssued) {
                    childSelectionIssued = writeBottomSelectionText(
                        dialogHwnd, L"平安下达生产订单方案V9306");
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(150));
            }
            selectedAfterNavigation = tools3000::dialog::DialogNavigator::getSelectedPath(dialogHwnd);
            selectionEmpty = selectedAfterNavigation.empty();
            if (mode == L"folder" && !selectedAfterNavigation.empty()) {
                childSelectionMatches = pathEquals(
                    std::filesystem::path(
                        tools3000::core::WinUtils::utf8ToWstring(selectedAfterNavigation)),
                    selectedChild);
            }
            bottomAfter = readControlText(dialogHwnd, 1152, preserveSaveName);
            const bool folderMatches = !currentAfterNavigation.empty() && pathEquals(
                std::filesystem::path(tools3000::core::WinUtils::utf8ToWstring(currentAfterNavigation)), target);
            const bool bottomPreserved = bottomBefore == bottomAfter &&
                                         (mode != L"save" || bottomAfter == L"draft.txt");
            const bool selectionCorrect = mode == L"folder"
                ? childSelectionIssued && childSelectionMatches
                : selectionEmpty;
            success = navigateIssued && remainedOpen && selectionCorrect &&
                      folderMatches && bottomPreserved;
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
                  !reportedFolder.empty() && pathEquals(
                      std::filesystem::path(tools3000::core::WinUtils::utf8ToWstring(reportedFolder)), target);
        allSucceeded = allSucceeded && success;

        std::cout << "mode=" << tools3000::core::WinUtils::wstringToUtf8(mode)
                  << " dialog_opened=" << (dialogHwnd != nullptr)
                  << " recognized=" << recognized
                  << " navigate_issued=" << navigateIssued
                  << " remained_open=" << remainedOpen
                  << " selection_empty=" << selectionEmpty
                  << " child_selection_issued=" << childSelectionIssued
                  << " selected_path=" << selectedAfterNavigation
                  << " child_selection_matches=" << childSelectionMatches
                  << " bottom_before=" << tools3000::core::WinUtils::wstringToUtf8(bottomBefore)
                  << " bottom_after=" << tools3000::core::WinUtils::wstringToUtf8(bottomAfter)
                  << " navigator_folder=" << currentAfterNavigation
                  << " child_exit=" << childExit
                  << " reported_folder=" << reportedFolder
                  << " expected_folder=" << tools3000::core::WinUtils::wstringToUtf8(target.native())
                  << " result=" << (success ? "PASS" : "FAIL") << '\n';
    }

    std::filesystem::remove_all(probeRoot, ec);
    return allSucceeded ? 0 : 23;
}
