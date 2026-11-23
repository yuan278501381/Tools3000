/**
 * Tools3000 - High Performance Windows Productivity Suite
 *
 * Copyright (c) 2026 Yy1 (GitHub yuan278501381) <https://github.com/yuan278501381> & Tools3000 contributors
 *
 * Licensed under the GPL-3.0 License.
 */

#include "DialogNavigator.h"
#include "core/events/MainThreadDispatcher.h"
#include "core/logger/Logger.h"
#include "core/utils/WinUtils.h"

#include <shlobj.h>
#include <shlwapi.h>
#include <uiautomation.h>
#include <wrl/client.h>

#include <algorithm>
#include <chrono>
#include <cwctype>
#include <filesystem>
#include <string_view>
#include <thread>
#include <vector>

#pragma comment(lib, "Shell32.lib")
#pragma comment(lib, "Shlwapi.lib")
#pragma comment(lib, "UIAutomationCore.lib")
#pragma comment(lib, "ole32.lib")

namespace tools3000::dialog {

using Microsoft::WRL::ComPtr;

DialogNavigator& DialogNavigator::instance() {
    static DialogNavigator s_instance;
    return s_instance;
}

DialogNavigator::~DialogNavigator() {
    shutdownAsyncWorker();
}

DialogType DialogNavigator::architectureToDialogType(DialogArchitecture arch) noexcept {
    switch (arch) {
        case DialogArchitecture::ModernIFileDialog:
            return DialogType::Modern;
        case DialogArchitecture::LegacyExplorer:
        case DialogArchitecture::ClassicOpenFileName:
            return DialogType::Legacy;
        case DialogArchitecture::BrowseForFolder:
            return DialogType::FolderPicker;
        case DialogArchitecture::Unknown:
        default:
            return DialogType::Unknown;
    }
}

// ============================================================
// 内部工具命名空间
// ============================================================
namespace {

constexpr UINT kCrossProcessTimeoutMs = 150;
constexpr UINT kUncProbeTimeoutMs = 200;

bool sendMessageWithTimeout(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam,
                            LRESULT& result, UINT timeoutMs = kCrossProcessTimeoutMs) {
    DWORD_PTR rawResult = 0;
    if (!hwnd || !SendMessageTimeoutW(hwnd, message, wParam, lParam,
                                      SMTO_ABORTIFHUNG | SMTO_BLOCK,
                                      timeoutMs, &rawResult)) {
        result = 0;
        return false;
    }
    result = static_cast<LRESULT>(rawResult);
    return true;
}

std::wstring getWindowTextWithTimeout(HWND hwnd, size_t maxChars = MAX_PATH * 2,
                                      UINT timeoutMs = kCrossProcessTimeoutMs) {
    if (!hwnd || maxChars < 2) return {};
    std::wstring value(maxChars, L'\0');
    LRESULT copied = 0;
    if (!sendMessageWithTimeout(hwnd, WM_GETTEXT, static_cast<WPARAM>(maxChars),
                                reinterpret_cast<LPARAM>(value.data()), copied, timeoutMs)) {
        return {};
    }
    if (copied <= 0) return {};
    value.resize((std::min)(static_cast<size_t>(copied), maxChars - 1));
    return value;
}

std::wstring normalizeComparePathW(std::wstring path) {
    if (path.empty()) return {};
    for (auto& ch : path) {
        if (ch == L'/') ch = L'\\';
    }
    std::filesystem::path p(path);
    std::wstring norm = p.lexically_normal().native();
    while (norm.size() > 3 && norm.back() == L'\\') {
        norm.pop_back();
    }
    return norm;
}

bool pathsEqualIgnoreCaseW(const std::wstring& a, const std::wstring& b) {
    const std::wstring na = normalizeComparePathW(a);
    const std::wstring nb = normalizeComparePathW(b);
    if (na.empty() || nb.empty()) return false;
    return _wcsicmp(na.c_str(), nb.c_str()) == 0;
}

bool isRemoteUncPath(const std::wstring& path) noexcept {
    if (path.size() < 3) return false;
    if ((path[0] == L'\\' || path[0] == L'/') && (path[1] == L'\\' || path[1] == L'/')) {
        if (path[2] == L'.') return false;
        if (path[2] == L'?') {
            if (path.size() >= 6 && std::iswalpha(path[4]) && path[5] == L':') {
                return false;
            }
            return true;
        }
        return true;
    }
    return false;
}

bool hasPlausibleUncShareSyntax(const std::wstring& path) noexcept {
    if (!isRemoteUncPath(path)) return false;
    size_t pos = 2;
    if (path.rfind(L"\\\\?\\UNC\\", 0) == 0 || path.rfind(L"\\\\?\\unc\\", 0) == 0) {
        pos = 8;
    }
    const size_t slash = path.find_first_of(L"\\/", pos);
    if (slash == std::wstring::npos || slash == pos || slash + 1 >= path.size()) {
        return false;
    }
    return path[slash + 1] != L'\\' && path[slash + 1] != L'/';
}

bool isDirectoryFastNonBlocking(const std::wstring& candidate) noexcept {
    if (candidate.empty()) return false;
    if (isRemoteUncPath(candidate)) {
        return hasPlausibleUncShareSyntax(candidate);
    }
    const DWORD attributes = GetFileAttributesW(candidate.c_str());
    return attributes != INVALID_FILE_ATTRIBUTES &&
           (attributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
}

bool isPathExistingFastNonBlocking(const std::wstring& candidate) noexcept {
    if (candidate.empty()) return false;
    if (isRemoteUncPath(candidate)) {
        return hasPlausibleUncShareSyntax(candidate);
    }
    return GetFileAttributesW(candidate.c_str()) != INVALID_FILE_ATTRIBUTES;
}

bool isVisibleNonEmptyRect(HWND hwnd) noexcept {
    if (!hwnd || !IsWindowVisible(hwnd)) return false;
    RECT bounds{};
    return GetWindowRect(hwnd, &bounds) && bounds.right > bounds.left &&
           bounds.bottom > bounds.top;
}

bool isEditReadOnly(HWND hwnd) noexcept {
    if (!hwnd) return true;
    const LONG_PTR style = GetWindowLongPtrW(hwnd, GWL_STYLE);
    return (style & ES_READONLY) != 0;
}

bool isAddressBandAncestor(HWND hwnd) noexcept {
    HWND parent = GetParent(hwnd);
    for (int depth = 0; depth < 5 && parent; ++depth, parent = GetParent(parent)) {
        wchar_t cls[64]{};
        if (GetClassNameW(parent, cls, static_cast<int>(std::size(cls))) > 0) {
            if (wcsstr(cls, L"Address") != nullptr ||
                wcsstr(cls, L"Breadcrumb") != nullptr ||
                wcsstr(cls, L"ReBar") != nullptr ||
                wcscmp(cls, L"WorkerW") == 0) {
                return true;
            }
        }
    }
    return false;
}

bool isDirectionalMark(wchar_t value) noexcept {
    return value == 0x200E || value == 0x200F ||
           (value >= 0x202A && value <= 0x202E) ||
           (value >= 0x2066 && value <= 0x2069);
}

std::wstring sanitizeShellText(std::wstring value) {
    value.erase(std::remove_if(value.begin(), value.end(), isDirectionalMark), value.end());
    while (!value.empty() && std::iswspace(value.back())) value.pop_back();
    return value;
}

std::wstring extractExistingDirectory(const std::wstring& shellText) {
    const std::wstring text = sanitizeShellText(shellText);
    for (size_t index = 0; index + 2 < text.size(); ++index) {
        const bool driveRoot = std::iswalpha(text[index]) && text[index + 1] == L':' &&
                               (text[index + 2] == L'\\' || text[index + 2] == L'/');
        const bool uncRoot = text[index] == L'\\' && text[index + 1] == L'\\' &&
                             index + 3 < text.size();
        if (!driveRoot && !uncRoot) continue;
        std::wstring candidate = text.substr(index);
        if (isDirectoryFastNonBlocking(candidate)) return candidate;
    }
    return {};
}

// 进程级只读缓存 comdlg32.dll 的系统 MUI 标准“打开/保存”按钮资源文案（0 代码执行，安全回退）
struct SystemComDlgStrings {
    std::wstring openButton;
    std::wstring saveButton;
};

const SystemComDlgStrings& getSystemComDlgStrings() {
    static const SystemComDlgStrings cached = []() {
        SystemComDlgStrings result;
        HMODULE mod = LoadLibraryExW(
            L"comdlg32.dll", nullptr,
            LOAD_LIBRARY_AS_DATAFILE | LOAD_LIBRARY_AS_IMAGE_RESOURCE);
        if (!mod) return result;

        auto loadSafeLabel = [&](UINT resId) -> std::wstring {
            wchar_t buf[64]{};
            const int len = LoadStringW(mod, resId, buf, static_cast<int>(std::size(buf)));
            if (len <= 0 || len >= 40) return {};
            std::wstring s(buf, static_cast<size_t>(len));
            if (s.find(L'%') != std::wstring::npos || s.find(L'\n') != std::wstring::npos) {
                return {};
            }
            return s;
        };

        // comdlg32.dll 标准内置按钮资源 ID（388 = &Open, 389 = &Save）
        result.openButton = loadSafeLabel(388);
        result.saveButton = loadSafeLabel(389);
        FreeLibrary(mod);
        return result;
    }();
    return cached;
}

// 递归穿透 ComboBoxEx32 -> ComboBox -> Edit 层级，精确提取真实可编辑子 Edit
HWND findEditableChildEditRecursive(HWND parent, bool requireVisible) noexcept {
    if (!parent || !IsWindow(parent)) return nullptr;

    struct SearchCtx {
        HWND result{nullptr};
        bool requireVisible{true};
    } ctx{nullptr, requireVisible};

    EnumChildWindows(parent, [](HWND child, LPARAM lParam) -> BOOL {
        auto* c = reinterpret_cast<SearchCtx*>(lParam);
        wchar_t cls[32]{};
        if (GetClassNameW(child, cls, static_cast<int>(std::size(cls))) > 0 &&
            wcscmp(cls, L"Edit") == 0) {
            if (isEditReadOnly(child)) return TRUE;
            if (c->requireVisible && !IsWindowVisible(child)) return TRUE;
            c->result = child;
            return FALSE;
        }
        return TRUE;
    }, reinterpret_cast<LPARAM>(&ctx));

    return ctx.result;
}

bool isTargetForeground(HWND dialogHwnd) noexcept {
    const HWND foreground = GetForegroundWindow();
    return foreground == dialogHwnd ||
           (foreground && GetAncestor(foreground, GA_ROOT) == dialogHwnd);
}

bool ensureTargetForeground(HWND dialogHwnd, uint64_t navEpoch = 0) {
    if (isTargetForeground(dialogHwnd)) return true;
    SetForegroundWindow(dialogHwnd);
    for (int retry = 0; retry < 8; ++retry) {
        if (!DialogNavigator::instance().waitInterruptibleMs(10, navEpoch)) return false;
        if (isTargetForeground(dialogHwnd)) return true;
    }
    return false;
}

bool sendKeyChord(HWND dialogHwnd, WORD modifier, WORD key, uint64_t navEpoch = 0) {
    if (!ensureTargetForeground(dialogHwnd, navEpoch)) return false;
    INPUT input[4]{};
    UINT count = 0;
    if (modifier != 0) {
        input[count].type = INPUT_KEYBOARD;
        input[count].ki.wVk = modifier;
        input[count].ki.wScan = static_cast<WORD>(MapVirtualKeyW(modifier, MAPVK_VK_TO_VSC));
        count++;
    }
    input[count].type = INPUT_KEYBOARD;
    input[count].ki.wVk = key;
    input[count].ki.wScan = static_cast<WORD>(MapVirtualKeyW(key, MAPVK_VK_TO_VSC));
    count++;
    input[count].type = INPUT_KEYBOARD;
    input[count].ki.wVk = key;
    input[count].ki.wScan = static_cast<WORD>(MapVirtualKeyW(key, MAPVK_VK_TO_VSC));
    input[count++].ki.dwFlags = KEYEVENTF_KEYUP;
    if (modifier != 0) {
        input[count].type = INPUT_KEYBOARD;
        input[count].ki.wVk = modifier;
        input[count].ki.wScan = static_cast<WORD>(MapVirtualKeyW(modifier, MAPVK_VK_TO_VSC));
        input[count++].ki.dwFlags = KEYEVENTF_KEYUP;
    }
    const UINT sent = SendInput(count, input, sizeof(INPUT));
    if (sent < count && modifier != 0) {
        if (modifier == VK_MENU || modifier == VK_LMENU || modifier == VK_RMENU) {
            INPUT neutral[2]{};
            neutral[0].type = INPUT_KEYBOARD;
            neutral[0].ki.wVk = VK_F24;
            neutral[1].type = INPUT_KEYBOARD;
            neutral[1].ki.wVk = VK_F24;
            neutral[1].ki.dwFlags = KEYEVENTF_KEYUP;
            SendInput(2, neutral, sizeof(INPUT));
        }
        INPUT up{};
        up.type = INPUT_KEYBOARD;
        up.ki.wVk = modifier;
        up.ki.wScan = static_cast<WORD>(MapVirtualKeyW(modifier, MAPVK_VK_TO_VSC));
        up.ki.dwFlags = KEYEVENTF_KEYUP;
        SendInput(1, &up, sizeof(INPUT));
    }
    return sent == count;
}

class UiaThreadContext final {
public:
    UiaThreadContext() noexcept {
        m_initializeResult = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        if (FAILED(m_initializeResult) && m_initializeResult != RPC_E_CHANGED_MODE) return;

        const HRESULT createResult = CoCreateInstance(
            CLSID_CUIAutomation, nullptr, CLSCTX_INPROC_SERVER,
            IID_IUIAutomation, reinterpret_cast<void**>(m_uia.GetAddressOf()));
        if (FAILED(createResult)) m_uia.Reset();
    }

    ~UiaThreadContext() noexcept {
        m_uia.Reset();
        if (SUCCEEDED(m_initializeResult)) CoUninitialize();
    }

    UiaThreadContext(const UiaThreadContext&) = delete;
    UiaThreadContext& operator=(const UiaThreadContext&) = delete;

    IUIAutomation* get() const noexcept { return m_uia.Get(); }

private:
    HRESULT m_initializeResult{E_UNEXPECTED};
    ComPtr<IUIAutomation> m_uia;
};

IUIAutomation* getUIA() noexcept {
    static thread_local UiaThreadContext context;
    return context.get();
}

std::wstring uiaGetValue(IUIAutomationElement* elem) {
    if (!elem) return L"";
    ComPtr<IUIAutomationValuePattern> vp;
    if (FAILED(elem->GetCurrentPatternAs(UIA_ValuePatternId, IID_IUIAutomationValuePattern,
                                          reinterpret_cast<void**>(vp.GetAddressOf()))) || !vp) {
        return L"";
    }
    BSTR val = nullptr;
    if (FAILED(vp->get_CurrentValue(&val)) || !val) return L"";
    std::wstring result(val);
    SysFreeString(val);
    return result;
}

bool isTitleProgressKeyword(std::wstring_view tv) noexcept {
    static constexpr std::wstring_view kProgressTitleKeywords[] = {
        L"正在复制", L"正在移动", L"正在删除", L"正在准备", L"正在计算", L"正在撤消", L"正在还原",
        L"正在同步", L"正在传输", L"正在解压", L"正在压缩", L"正在清理",
        L"文件复制", L"文件移动", L"文件删除", L"复制进度", L"移动进度", L"删除进度", L"传输进度", L"操作进度",
        L"Copying", L"copying", L"Moving", L"moving", L"Deleting", L"deleting",
        L"Preparing to copy", L"Preparing to move", L"Calculating", L"calculating",
        L"Transferring", L"transferring", L"Extracting", L"extracting",
        L"Operation Status", L"Copy Progress", L"Move Progress", L"File Transfer",
        L"コピー中", L"移動中", L"削除中",
        L"Kopiervorgang", L"Verschiebevorgang", L"Löschvorgang",
        L"Copie en cours", L"Déplacement en cours", L"Suppression en cours",
        L"Copiando", L"Moviendo", L"Eliminando",
        L"Копирование", L"Перемещение", L"Удаление"
    };
    for (const auto& keyword : kProgressTitleKeywords) {
        if (tv.find(keyword) != std::wstring_view::npos) return true;
    }
    return false;
}

bool isTitlePropertySheetSuffix(std::wstring_view tv) noexcept {
    static constexpr std::wstring_view kPropertySuffixes[] = {
        L" 属性", L"属性",
        L" Properties", L"Properties",
        L" Property", L"Property",
        L" 屬性", L"屬性",
        L" 內容", L"內容",
        L" のプロパティ", L" プロパティ", L"プロパティ",
        L" 속성", L"속성",
        L" Eigenschaften", L"Eigenschaften",
        L" Propriétés", L"Propriétés",
        L" Propiedades", L"Propiedades",
        L" Proprietà", L"Proprietà",
        L" Propriedades", L"Propriedades",
        L" Свойства", L"Свойства"
    };
    for (const auto& suffix : kPropertySuffixes) {
        if (tv.ends_with(suffix)) return true;
    }
    return false;
}

bool isTitleWizardKeyword(std::wstring_view tv) noexcept {
    static constexpr std::wstring_view kWizardTitleKeywords[] = {
        L"安装向导", L"设置向导", L"更新向导", L"升级向导", L"卸载向导",
        L"安装程序", L"卸载程序", L"安装", L"向导", L"卸载",
        L"Setup Wizard", L"Installation Wizard", L"Update Wizard", L"Setup",
        L"Installer", L"Installation", L"Wizard", L"Uninstall", L"Uninstaller",
        L"Destination Folder", L"Select Destination", L"Choose Install",
        L"Installation Folder", L"Install Location", L"安装目录", L"安装位置",
        L"目標資料夾", L"安裝精靈", L"安裝程式", L"安裝",
        L"セットアップ", L"インストール", L"ウィザード", L"アンインストール",
        L"Installations-Assistent", L"Assistent", L"Désinstallation", L"Assistant",
        L"Asistente para la instalación", L"Asistente", L"Мастер установки"
    };
    for (const auto& kw : kWizardTitleKeywords) {
        if (tv.find(kw) != std::wstring_view::npos) return true;
    }
    return false;
}

bool isBackButtonLabel(std::wstring_view bv) noexcept {
    return bv.find(L"上一步") != std::wstring_view::npos ||
           bv.find(L"Back") != std::wstring_view::npos ||
           bv.find(L"back") != std::wstring_view::npos ||
           bv.find(L"<") != std::wstring_view::npos ||
           bv.find(L"戻る") != std::wstring_view::npos ||
           bv.find(L"Zurück") != std::wstring_view::npos ||
           bv.find(L"Précédent") != std::wstring_view::npos ||
           bv.find(L"Назад") != std::wstring_view::npos;
}

bool isProgressStaticLabel(std::wstring_view sv) noexcept {
    return sv.find(L"正在复制") != std::wstring_view::npos ||
           sv.find(L"正在移动") != std::wstring_view::npos ||
           sv.find(L"正在删除") != std::wstring_view::npos ||
           sv.find(L"正在准备") != std::wstring_view::npos ||
           sv.find(L"正在计算") != std::wstring_view::npos ||
           sv.find(L"正在传输") != std::wstring_view::npos ||
           sv.find(L"正在解压") != std::wstring_view::npos ||
           sv.find(L"正在压缩") != std::wstring_view::npos ||
           sv.find(L"Copying") != std::wstring_view::npos ||
           sv.find(L"Moving") != std::wstring_view::npos ||
           sv.find(L"Deleting") != std::wstring_view::npos ||
           sv.find(L"Transferring") != std::wstring_view::npos ||
           sv.find(L"Extracting") != std::wstring_view::npos;
}

bool isInstallerWindowClass(HWND hwnd) noexcept {
    if (!hwnd || !IsWindow(hwnd)) return false;
    wchar_t cls[64]{};
    if (GetClassNameW(hwnd, cls, static_cast<int>(std::size(cls))) <= 0) return false;
    return wcsstr(cls, L"TSetupForm") != nullptr ||
           wcsstr(cls, L"TWizardForm") != nullptr ||
           wcsstr(cls, L"TFolderTreeView") != nullptr ||
           wcsstr(cls, L"TNewNotebook") != nullptr ||
           wcsstr(cls, L"WixBurn") != nullptr ||
           wcsstr(cls, L"InstallShield") != nullptr ||
           wcsstr(cls, L"MsiDialogCloseClass") != nullptr;
}

std::string uiaGetSelectedShellChild(HWND dialogHwnd, const std::string& currentFolder) {
    if (currentFolder.empty()) return {};

    IUIAutomation* uia = getUIA();
    if (!uia) return {};

    ComPtr<IUIAutomationElement> dialogElement;
    if (FAILED(uia->ElementFromHandle(dialogHwnd, dialogElement.GetAddressOf())) || !dialogElement) {
        return {};
    }

    VARIANT selectedValue{};
    selectedValue.vt = VT_BOOL;
    selectedValue.boolVal = VARIANT_TRUE;
    ComPtr<IUIAutomationCondition> selectedCondition;
    if (FAILED(uia->CreatePropertyCondition(
            UIA_SelectionItemIsSelectedPropertyId, selectedValue,
            selectedCondition.GetAddressOf())) || !selectedCondition) {
        return {};
    }

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
        return {};
    }

    ComPtr<IUIAutomationCondition> selectedShellItemCondition;
    if (FAILED(uia->CreateAndCondition(
            selectedCondition.Get(), shellItemTypeCondition.Get(),
            selectedShellItemCondition.GetAddressOf())) || !selectedShellItemCondition) {
        return {};
    }

    ComPtr<IUIAutomationElement> selectedElement;
    if (FAILED(dialogElement->FindFirst(
            TreeScope_Descendants, selectedShellItemCondition.Get(),
            selectedElement.GetAddressOf())) || !selectedElement) {
        return {};
    }

    BSTR rawName = nullptr;
    if (FAILED(selectedElement->get_CurrentName(&rawName)) || !rawName) return {};
    const std::wstring itemName = sanitizeShellText(rawName);
    SysFreeString(rawName);
    if (itemName.empty() || itemName == L"." || itemName == L".." ||
        itemName.find_first_of(L"\\/") != std::wstring::npos) {
        return {};
    }

    const std::filesystem::path parent = tools3000::core::WinUtils::utf8ToPath(currentFolder);
    const std::filesystem::path candidate = (parent / itemName).lexically_normal();
    if (!isPathExistingFastNonBlocking(candidate.native())) return {};
    return tools3000::core::WinUtils::pathToUtf8(candidate);
}

// 查找地址栏 Edit 41477（通过严格归属与几何校验；preferVisible=true 时优先返回可见实例，
// 若面包屑态下 Edit 41477 处于常规隐藏态但满足全部几何校验，也可在 requireVisible=false 时返回以实现零焦点直达）
HWND findSafeAddressEdit41477(HWND dialogHwnd, const RECT& dialogRect, bool requireVisible) noexcept {
    const LONG dialogWidth = dialogRect.right - dialogRect.left;
    const LONG dialogHeight = dialogRect.bottom - dialogRect.top;
    if (dialogWidth <= 0 || dialogHeight <= 0) return nullptr;

    struct SearchCtx {
        HWND dialogHwnd;
        RECT dialogRect;
        LONG dialogWidth;
        LONG dialogHeight;
        bool requireVisible;
        HWND visibleFound{nullptr};
        HWND hiddenFound{nullptr};
    } ctx{dialogHwnd, dialogRect, dialogWidth, dialogHeight, requireVisible, nullptr, nullptr};

    EnumChildWindows(dialogHwnd, [](HWND child, LPARAM param) -> BOOL {
        auto* c = reinterpret_cast<SearchCtx*>(param);
        if (GetDlgCtrlID(child) != 41477) return TRUE;
        const bool isVis = IsWindowVisible(child) != FALSE;
        if (c->requireVisible && !isVis) return TRUE;
        wchar_t className[32]{};
        if (GetClassNameW(child, className, static_cast<int>(std::size(className))) <= 0 ||
            wcscmp(className, L"Edit") != 0) {
            return TRUE;
        }
        RECT editRect{};
        if (!GetWindowRect(child, &editRect)) return TRUE;
        const LONG width = editRect.right - editRect.left;
        const LONG height = editRect.bottom - editRect.top;
        const LONG centerX = editRect.left + width / 2;
        const bool belongs = (GetAncestor(child, GA_ROOT) == c->dialogHwnd || IsChild(c->dialogHwnd, child));
        const bool inUpperBand = editRect.top >= c->dialogRect.top &&
                                 editRect.bottom <= c->dialogRect.top + c->dialogHeight * 45 / 100;
        const bool notSearchBox = centerX <= c->dialogRect.left + c->dialogWidth * 72 / 100;
        const bool usableSize = width >= 80 && height >= 12;
        if (belongs && inUpperBand && notSearchBox && usableSize) {
            if (isVis) {
                c->visibleFound = child;
                return FALSE;
            }
            if (!c->hiddenFound) {
                c->hiddenFound = child;
            }
        }
        return TRUE;
    }, reinterpret_cast<LPARAM>(&ctx));

    return ctx.visibleFound ? ctx.visibleFound : (requireVisible ? nullptr : ctx.hiddenFound);
}

HWND findSafeVisibleAddressEdit41477(HWND dialogHwnd, const RECT& dialogRect) noexcept {
    return findSafeAddressEdit41477(dialogHwnd, dialogRect, true);
}

// 静默点击 Breadcrumb Parent 下的 ToolbarWindow32 右侧空白区激活 Edit 41477（~5ms，0 全局 SendInput）
bool activateAddressEditSilently(HWND dialogHwnd, const DialogControlSnapshot& snap) noexcept {
    HWND targetBar = nullptr;
    if (snap.addressToolbarHwnd && IsWindow(snap.addressToolbarHwnd) &&
        isVisibleNonEmptyRect(snap.addressToolbarHwnd)) {
        targetBar = snap.addressToolbarHwnd;
    } else if (snap.breadcrumbParentHwnd && IsWindow(snap.breadcrumbParentHwnd) &&
               isVisibleNonEmptyRect(snap.breadcrumbParentHwnd)) {
        HWND tb = FindWindowExW(snap.breadcrumbParentHwnd, nullptr, L"ToolbarWindow32", nullptr);
        targetBar = (tb && isVisibleNonEmptyRect(tb)) ? tb : snap.breadcrumbParentHwnd;
    }

    if (!targetBar) {
        // 动态定位一次可见的地址栏 ToolbarWindow32
        struct FindBarCtx { HWND found{nullptr}; } findCtx;
        EnumChildWindows(dialogHwnd, [](HWND child, LPARAM param) -> BOOL {
            auto* c = reinterpret_cast<FindBarCtx*>(param);
            if (!isVisibleNonEmptyRect(child)) return TRUE;
            wchar_t cls[64]{};
            if (GetClassNameW(child, cls, static_cast<int>(std::size(cls))) <= 0) return TRUE;
            if (wcscmp(cls, L"ToolbarWindow32") == 0 && isAddressBandAncestor(child)) {
                c->found = child;
                return FALSE;
            }
            return TRUE;
        }, reinterpret_cast<LPARAM>(&findCtx));
        targetBar = findCtx.found;
    }

    if (!targetBar || !IsWindow(targetBar)) return false;

    RECT clientRc{};
    if (!GetClientRect(targetBar, &clientRc)) return false;
    const int width = clientRc.right - clientRc.left;
    const int height = clientRc.bottom - clientRc.top;
    if (width < 24 || height < 8) return false;

    const int clickX = (std::max)(4, width - 12);
    const int clickY = (std::max)(2, height / 2);
    const LPARAM lParam = MAKELPARAM(clickX, clickY);

    if (!PostMessageW(targetBar, WM_LBUTTONDOWN, MK_LBUTTON, lParam)) {
        return false;
    }
    PostMessageW(targetBar, WM_LBUTTONUP, 0, lParam);
    return true;
}

}  // namespace

// ============================================================
// 单次 EnumChildWindows 完整快照采集
// ============================================================
DialogControlSnapshot DialogNavigator::inspectDialog(HWND dialogHwnd, bool allowCurrentProcess) {
    DialogControlSnapshot snap;
    snap.dialogHwnd = dialogHwnd;
    if (!dialogHwnd || !IsWindow(dialogHwnd)) {
        return snap;
    }

    wchar_t rootClass[64]{};
    GetClassNameW(dialogHwnd, rootClass, static_cast<int>(std::size(rootClass)));
    if (_wcsicmp(rootClass, L"OperationStatusWindow") == 0) {
        snap.isProgressOrTransfer = true;
        return snap;
    }
    HWND root = GetAncestor(dialogHwnd, GA_ROOT);
    if (root && root != dialogHwnd) {
        wchar_t ancClass[64]{};
        if (GetClassNameW(root, ancClass, static_cast<int>(std::size(ancClass))) > 0 &&
            _wcsicmp(ancClass, L"OperationStatusWindow") == 0) {
            snap.isProgressOrTransfer = true;
            return snap;
        }
    }

    if (wcscmp(rootClass, L"#32770") != 0) {
        return snap;
    }

    DWORD pid = 0;
    GetWindowThreadProcessId(dialogHwnd, &pid);
    if (pid == 0 || (!allowCurrentProcess && pid == GetCurrentProcessId())) {
        return snap;
    }

    snap.dialogTitle = getWindowTextWithTimeout(dialogHwnd, 256);
    if (isTitleProgressKeyword(snap.dialogTitle)) {
        snap.isProgressOrTransfer = true;
    }
    if (isTitlePropertySheetSuffix(snap.dialogTitle) ||
        GetDlgItem(dialogHwnd, 0x3021) != nullptr ||
        GetDlgItem(dialogHwnd, 12321) != nullptr) {
        snap.isPropertySheet = true;
        snap.signals.hasApplyButton = true;
    }

    struct ScanState {
        DialogControlSnapshot* snap;
        HWND bestVisibleFileEdit{nullptr};
        HWND bestAnyFileEdit{nullptr};
        LONG bestVisibleEditTop{LONG_MIN};
        LONG bestAnyEditTop{LONG_MIN};
        bool hasProgressBar{false};
        bool hasAnimation{false};
        bool hasProgressText{false};
    } state{&snap};

    EnumChildWindows(dialogHwnd, [](HWND child, LPARAM lParam) -> BOOL {
        auto* st = reinterpret_cast<ScanState*>(lParam);
        auto& s = *st->snap;
        s.signals.totalControls++;

        wchar_t cls[64]{};
        GetClassNameW(child, cls, static_cast<int>(std::size(cls)));
        const int ctrlId = GetDlgCtrlID(child);

        if (wcscmp(cls, L"SHELLDLL_DefView") == 0) {
            s.signals.hasDefView = true;
            if (!s.shellViewHwnd) s.shellViewHwnd = child;
        } else if (wcscmp(cls, L"DirectUIHWND") == 0 || wcscmp(cls, L"DUIViewWndClassName") == 0) {
            s.signals.hasDirectUI = true;
        } else if (wcscmp(cls, L"Address Band Root") == 0) {
            s.signals.hasAddressBand = true;
        } else if (wcscmp(cls, L"Breadcrumb Parent") == 0) {
            s.signals.hasAddressBand = true;
            if (!s.breadcrumbParentHwnd) s.breadcrumbParentHwnd = child;
        } else if (wcscmp(cls, L"ToolbarWindow32") == 0) {
            if (isAddressBandAncestor(child)) {
                s.signals.hasAddressBand = true;
                if (!s.addressToolbarHwnd || isVisibleNonEmptyRect(child)) {
                    s.addressToolbarHwnd = child;
                }
            }
        } else if (wcscmp(cls, L"NamespaceTreeControl") == 0 || wcscmp(cls, L"SysTreeView32") == 0) {
            s.signals.hasTreeView = true;
        } else if (wcscmp(cls, L"SysTabControl32") == 0) {
            s.signals.hasTabControl = true;
            s.isPropertySheet = true;
        } else if (wcscmp(cls, L"msctls_progress32") == 0) {
            if (!isAddressBandAncestor(child)) {
                st->hasProgressBar = true;
            }
        } else if (wcscmp(cls, L"SysAnimate32") == 0) {
            st->hasAnimation = true;
        } else if (wcscmp(cls, L"ComboBoxEx32") == 0 || wcscmp(cls, L"ComboBox") == 0) {
            s.signals.hasAnyComboBox = true;
            if (ctrlId == 1136 || ctrlId == 0x0470) {
                s.signals.hasFileTypeCombo1136 = true;
                if (!s.fileTypeComboHwnd) s.fileTypeComboHwnd = child;
            } else if (ctrlId == 1148 || ctrlId == 0x047C) {
                s.signals.hasFileNameCombo1148 = true;
                if (!s.fileNameComboHwnd) s.fileNameComboHwnd = child;
            }
        } else if (wcscmp(cls, L"Edit") == 0) {
            if (ctrlId == 41477) {
                if (!s.addressEdit41477 || IsWindowVisible(child)) {
                    s.addressEdit41477 = child;
                }
            } else if (ctrlId == 1152 || ctrlId == 0x0480 || ctrlId == 0x047C) {
                s.signals.hasFileNameEdit1152 = true;
                if (!s.fileNameEditHwnd || (IsWindowVisible(child) && !isEditReadOnly(child))) {
                    s.fileNameEditHwnd = child;
                }
            } else if (!isEditReadOnly(child)) {
                RECT rc{};
                if (GetWindowRect(child, &rc)) {
                    if (IsWindowVisible(child) && rc.top > st->bestVisibleEditTop) {
                        st->bestVisibleEditTop = rc.top;
                        st->bestVisibleFileEdit = child;
                    }
                    if (rc.top > st->bestAnyEditTop) {
                        st->bestAnyEditTop = rc.top;
                        st->bestAnyFileEdit = child;
                    }
                }
            }
        } else if (wcscmp(cls, L"Button") == 0) {
            if (ctrlId == 0x3021 || ctrlId == 12321) {
                s.signals.hasApplyButton = true;
                s.isPropertySheet = true;
            } else if (ctrlId == 0x3023 || ctrlId == 12323 || ctrlId == 1002 || ctrlId == 1028 || ctrlId == 1044) {
                s.signals.hasBackButton = true;
                s.backButtonHwnd = child;
            } else if (ctrlId == IDOK || ctrlId == 1 || ctrlId == 0x0400) {
                s.signals.hasOkButton = true;
                if (!s.okButtonHwnd || ctrlId == IDOK) {
                    s.okButtonHwnd = child;
                }
            } else if (ctrlId == IDCANCEL || ctrlId == 2) {
                s.signals.hasCancelButton = true;
                if (!s.cancelButtonHwnd) s.cancelButtonHwnd = child;
            }

            const std::wstring btnText = getWindowTextWithTimeout(child, 64);
            if (!btnText.empty()) {
                if (child == s.okButtonHwnd) {
                    s.okButtonText = btnText;
                }
                if (isBackButtonLabel(btnText)) {
                    s.signals.hasBackButton = true;
                    s.backButtonHwnd = child;
                }
            }
        } else if (wcscmp(cls, L"Static") == 0) {
            const std::wstring text = getWindowTextWithTimeout(child, 128);
            if (!text.empty() && isProgressStaticLabel(text)) {
                st->hasProgressText = true;
            }
        }
        return TRUE;
    }, reinterpret_cast<LPARAM>(&state));

    // 若 cmb13 (1148) 存在但还没拿到直接 Edit 1152，则递归穿透 ComboBoxEx32 -> ComboBox -> Edit
    if (!snap.fileNameEditHwnd && snap.fileNameComboHwnd) {
        snap.fileNameEditHwnd = findEditableChildEditRecursive(snap.fileNameComboHwnd, true);
        if (!snap.fileNameEditHwnd) {
            snap.fileNameEditHwnd = findEditableChildEditRecursive(snap.fileNameComboHwnd, false);
        }
    }
    if (!snap.fileNameEditHwnd) {
        snap.fileNameEditHwnd = state.bestVisibleFileEdit ? state.bestVisibleFileEdit : state.bestAnyFileEdit;
    }

    const bool hasFileCore = snap.signals.hasDefView || snap.signals.hasTreeView;
    if (!hasFileCore && (state.hasProgressBar || state.hasAnimation || state.hasProgressText)) {
        snap.isProgressOrTransfer = true;
    }
    snap.signals.hasProgressIndicator = snap.isProgressOrTransfer ||
                                        state.hasProgressBar ||
                                        state.hasAnimation ||
                                        state.hasProgressText;

    // 架构分类（MECE 5 类）
    snap.architecture = classifyDialogArchitecture(snap.signals);

    // 向导/安装程序判定
    const std::string procName = tools3000::core::WinUtils::getProcessNameFromWindow(dialogHwnd);
    HWND rootOwner = GetAncestor(dialogHwnd, GA_ROOTOWNER);
    const std::string ownerProc = (rootOwner && rootOwner != dialogHwnd)
        ? tools3000::core::WinUtils::getProcessNameFromWindow(rootOwner)
        : std::string{};
    snap.isInstallerOrWizard =
        isInstallerProcess(procName) ||
        isInstallerProcess(ownerProc) ||
        isInstallerWindowClass(dialogHwnd) ||
        (rootOwner && isInstallerWindowClass(rootOwner)) ||
        isTitleWizardKeyword(snap.dialogTitle) ||
        snap.signals.hasBackButton;

    // 业务模式分类（OpenFile / SaveFile / PickFolder / Unknown）
    if (snap.architecture != DialogArchitecture::Unknown) {
        if (snap.okButtonText.empty() && snap.okButtonHwnd) {
            snap.okButtonText = getWindowTextWithTimeout(snap.okButtonHwnd, 64);
        }
        bool fileNameHasInitialText = false;
        if (snap.fileNameEditHwnd && IsWindow(snap.fileNameEditHwnd)) {
            LRESULT len = 0;
            if (sendMessageWithTimeout(snap.fileNameEditHwnd, WM_GETTEXTLENGTH, 0, 0, len) && len > 0) {
                fileNameHasInitialText = true;
            }
        }
        const auto& sysStrings = getSystemComDlgStrings();
        DialogModeSignals modeSignals;
        modeSignals.architecture = snap.architecture;
        modeSignals.hasDirectUI = snap.signals.hasDirectUI;
        modeSignals.hasDefView = snap.signals.hasDefView;
        modeSignals.hasFileTypeCombo1136 = snap.signals.hasFileTypeCombo1136;
        modeSignals.hasFileNameEdit1152 = snap.signals.hasFileNameEdit1152 || (snap.fileNameEditHwnd != nullptr);
        modeSignals.fileNameEditHasInitialText = fileNameHasInitialText;
        modeSignals.okButtonText = snap.okButtonText;
        modeSignals.dialogTitle = snap.dialogTitle;
        modeSignals.systemOpenButtonText = sysStrings.openButton;
        modeSignals.systemSaveButtonText = sysStrings.saveButton;
        snap.mode = classifyDialogMode(modeSignals);
    }

    return snap;
}

// ============================================================
// 识别与兼容接口（全部复用单次快照）
// ============================================================
bool DialogNavigator::isProgressOrTransferDialog(HWND hwnd) {
    if (!hwnd || !IsWindow(hwnd)) return false;

    wchar_t className[64]{};
    if (GetClassNameW(hwnd, className, static_cast<int>(std::size(className))) > 0 &&
        _wcsicmp(className, L"OperationStatusWindow") == 0) {
        return true;
    }

    HWND root = GetAncestor(hwnd, GA_ROOT);
    if (root && root != hwnd) {
        wchar_t rootClass[64]{};
        if (GetClassNameW(root, rootClass, static_cast<int>(std::size(rootClass))) > 0 &&
            _wcsicmp(rootClass, L"OperationStatusWindow") == 0) {
            return true;
        }
    }

    const std::wstring title = getWindowTextWithTimeout(hwnd, 256);
    if (isTitleProgressKeyword(title)) {
        return true;
    }

    struct ProgressScanCtx {
        bool hasProgressFeature{false};
        bool hasFileDialogCore{false};
    } scanCtx;

    EnumChildWindows(hwnd, [](HWND child, LPARAM lParam) -> BOOL {
        auto* ctx = reinterpret_cast<ProgressScanCtx*>(lParam);
        wchar_t childClass[64]{};
        GetClassNameW(child, childClass, static_cast<int>(std::size(childClass)));
        if (_wcsicmp(childClass, L"SHELLDLL_DefView") == 0 ||
            _wcsicmp(childClass, L"NamespaceTreeControl") == 0) {
            ctx->hasFileDialogCore = true;
            return TRUE;
        }
        if (_wcsicmp(childClass, L"msctls_progress32") == 0) {
            if (!isAddressBandAncestor(child)) {
                ctx->hasProgressFeature = true;
            }
            return TRUE;
        }
        if (_wcsicmp(childClass, L"SysAnimate32") == 0) {
            ctx->hasProgressFeature = true;
            return TRUE;
        }
        const std::wstring text = getWindowTextWithTimeout(child, 128);
        if (!text.empty() && isProgressStaticLabel(text)) {
            ctx->hasProgressFeature = true;
        }
        return TRUE;
    }, reinterpret_cast<LPARAM>(&scanCtx));

    return !scanCtx.hasFileDialogCore && scanCtx.hasProgressFeature;
}

bool DialogNavigator::isInstallerProcess(const std::string& processName) {
    if (processName.empty()) return false;
    std::string lower = processName;
    std::transform(lower.begin(), lower.end(), lower.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });

    static constexpr const char* kInstallerKeywords[] = {
        "setup", "install", "unins", "msiexec", "update", "patch", "deploy", "wizard",
        "bootstrapper", "downloader", "package", "extract"
    };
    for (const auto* kw : kInstallerKeywords) {
        if (lower.find(kw) != std::string::npos) return true;
    }
    return false;
}

bool DialogNavigator::isInstallerOrWizard(HWND dialogHwnd) {
    if (!dialogHwnd || !IsWindow(dialogHwnd)) return false;

    const std::string processName = tools3000::core::WinUtils::getProcessNameFromWindow(dialogHwnd);
    if (isInstallerProcess(processName)) return true;

    HWND rootOwner = GetAncestor(dialogHwnd, GA_ROOTOWNER);
    if (rootOwner && rootOwner != dialogHwnd) {
        const std::string ownerProc = tools3000::core::WinUtils::getProcessNameFromWindow(rootOwner);
        if (isInstallerProcess(ownerProc)) return true;
    }

    if (isInstallerWindowClass(dialogHwnd) || (rootOwner && isInstallerWindowClass(rootOwner))) {
        return true;
    }

    const std::wstring title = getWindowTextWithTimeout(dialogHwnd, 256);
    if (isTitleWizardKeyword(title)) {
        return true;
    }

    bool hasWizardControl = false;
    EnumChildWindows(dialogHwnd, [](HWND child, LPARAM lParam) -> BOOL {
        wchar_t cls[64]{};
        GetClassNameW(child, cls, static_cast<int>(std::size(cls)));
        if (wcscmp(cls, L"Button") == 0) {
            const int ctrlId = GetDlgCtrlID(child);
            if (ctrlId == 0x3023 || ctrlId == 12323 || ctrlId == 1002 || ctrlId == 1028 || ctrlId == 1044) {
                *reinterpret_cast<bool*>(lParam) = true;
                return FALSE;
            }
            const std::wstring text = getWindowTextWithTimeout(child, 64);
            if (!text.empty() && isBackButtonLabel(text)) {
                *reinterpret_cast<bool*>(lParam) = true;
                return FALSE;
            }
        }
        return TRUE;
    }, reinterpret_cast<LPARAM>(&hasWizardControl));

    return hasWizardControl;
}

DialogArchitecture DialogNavigator::detectDialogArchitecture(HWND hwnd, bool allowCurrentProcess) {
    return inspectDialog(hwnd, allowCurrentProcess).architecture;
}

DialogMode DialogNavigator::detectDialogMode(HWND hwnd, bool allowCurrentProcess) {
    return inspectDialog(hwnd, allowCurrentProcess).mode;
}

DialogType DialogNavigator::detectDialogType(HWND hwnd) {
    return architectureToDialogType(inspectDialog(hwnd, true).architecture);
}

bool DialogNavigator::isFileDialog(HWND hwnd, bool allowCurrentProcess) {
    const DialogControlSnapshot snap = inspectDialog(hwnd, allowCurrentProcess);
    if (snap.isProgressOrTransfer || snap.isPropertySheet) {
        return false;
    }
    return snap.architecture != DialogArchitecture::Unknown;
}

// ============================================================
// 子控件精准查找（修复 ComboBoxEx32 -> ComboBox -> Edit 层级）
// ============================================================
HWND DialogNavigator::findPathEditControl(HWND dialogHwnd) {
    if (!dialogHwnd || !IsWindow(dialogHwnd)) return nullptr;

    struct EditCandidates {
        HWND visibleEdt1152{nullptr};
        HWND anyEdt1152{nullptr};
        HWND visibleCmb1148Edit{nullptr};
        HWND anyCmb1148Edit{nullptr};
        HWND bestVisibleEdit{nullptr};
        HWND bestAnyEdit{nullptr};
        LONG bestVisibleTop{LONG_MIN};
        LONG bestAnyTop{LONG_MIN};
    } cand{};

    EnumChildWindows(dialogHwnd, [](HWND child, LPARAM lParam) -> BOOL {
        auto* c = reinterpret_cast<EditCandidates*>(lParam);
        wchar_t cls[64]{};
        if (GetClassNameW(child, cls, static_cast<int>(std::size(cls))) <= 0) return TRUE;
        const int ctrlId = GetDlgCtrlID(child);

        if (wcscmp(cls, L"Edit") == 0) {
            if (ctrlId == 41477 || isEditReadOnly(child)) return TRUE;
            const bool vis = IsWindowVisible(child) != FALSE;
            if (ctrlId == 1152 || ctrlId == 0x0480) {
                if (vis && !c->visibleEdt1152) c->visibleEdt1152 = child;
                if (!c->anyEdt1152) c->anyEdt1152 = child;
                return TRUE;
            }
            RECT rc{};
            if (GetWindowRect(child, &rc)) {
                if (vis && rc.top > c->bestVisibleTop) {
                    c->bestVisibleTop = rc.top;
                    c->bestVisibleEdit = child;
                }
                if (rc.top > c->bestAnyTop) {
                    c->bestAnyTop = rc.top;
                    c->bestAnyEdit = child;
                }
            }
        } else if (wcscmp(cls, L"ComboBoxEx32") == 0 || wcscmp(cls, L"ComboBox") == 0) {
            if (ctrlId == 1148 || ctrlId == 0x047C) {
                if (!c->visibleCmb1148Edit) {
                    c->visibleCmb1148Edit = findEditableChildEditRecursive(child, true);
                }
                if (!c->anyCmb1148Edit) {
                    c->anyCmb1148Edit = findEditableChildEditRecursive(child, false);
                }
            } else {
                HWND innerVis = findEditableChildEditRecursive(child, true);
                if (innerVis) {
                    RECT rc{};
                    if (GetWindowRect(innerVis, &rc) && rc.top > c->bestVisibleTop) {
                        c->bestVisibleTop = rc.top;
                        c->bestVisibleEdit = innerVis;
                    }
                }
            }
        }
        return TRUE;
    }, reinterpret_cast<LPARAM>(&cand));

    if (cand.visibleEdt1152) return cand.visibleEdt1152;
    if (cand.visibleCmb1148Edit) return cand.visibleCmb1148Edit;
    if (cand.anyEdt1152) return cand.anyEdt1152;
    if (cand.anyCmb1148Edit) return cand.anyCmb1148Edit;
    if (cand.bestVisibleEdit) return cand.bestVisibleEdit;
    return cand.bestAnyEdit;
}

HWND DialogNavigator::findAddressBandControl(HWND dialogHwnd) {
    return inspectDialog(dialogHwnd, true).addressToolbarHwnd;
}

HWND DialogNavigator::findShellViewControl(HWND dialogHwnd) {
    return inspectDialog(dialogHwnd, true).shellViewHwnd;
}

HWND DialogNavigator::findOkButtonControl(HWND dialogHwnd) {
    return inspectDialog(dialogHwnd, true).okButtonHwnd;
}

// ============================================================
// 受管单线程 UNC 异步探测槽（彻底消灭 std::thread::detach）
// ============================================================
void DialogNavigator::ensureProbeWorkerStarted() {
    std::lock_guard lock(m_probeMutex);
    if (m_probeRunning.load(std::memory_order_acquire)) return;
    if (m_probeWorker.joinable()) {
        m_probeWorker.join();
    }
    m_probeRunning.store(true, std::memory_order_release);
    m_probeWorker = std::thread([this]() noexcept {
        probeWorkerLoop();
    });
}

void DialogNavigator::probeWorkerLoop() {
    while (m_probeRunning.load(std::memory_order_acquire)) {
        std::optional<UncProbeRequest> req;
        {
            std::unique_lock lock(m_probeMutex);
            m_probeCv.wait(lock, [this]() {
                return !m_probeRunning.load(std::memory_order_acquire) || m_probeRequest.has_value();
            });
            if (!m_probeRunning.load(std::memory_order_acquire)) break;
            req = std::move(m_probeRequest);
            m_probeRequest.reset();
        }
        if (req && req->attrs && req->done) {
            const DWORD attrs = GetFileAttributesW(req->path.c_str());
            req->attrs->store(attrs, std::memory_order_release);
            req->done->store(true, std::memory_order_release);
            m_probeDoneCv.notify_all();
        }
    }
}

void DialogNavigator::shutdownProbeWorker() {
    m_probeRunning.store(false, std::memory_order_release);
    {
        std::lock_guard lock(m_probeMutex);
        m_probeRequest.reset();
    }
    m_probeCv.notify_all();
    m_probeDoneCv.notify_all();
    if (m_probeWorker.joinable() && m_probeWorker.get_id() != std::this_thread::get_id()) {
        // 如果后台探测正卡在离线 SMB 的 GetFileAttributesW，立即通过 CancelSynchronousIo 唤醒并安全 join
        HANDLE hThread = static_cast<HANDLE>(m_probeWorker.native_handle());
        if (hThread) {
            CancelSynchronousIo(hThread);
        }
        m_probeWorker.join();
    }
}

bool DialogNavigator::verifyNavigableDirectoryBounded(const std::wstring& candidate) {
    if (candidate.empty()) return false;
    if (!isRemoteUncPath(candidate)) {
        const DWORD attrs = GetFileAttributesW(candidate.c_str());
        return attrs != INVALID_FILE_ATTRIBUTES && (attrs & FILE_ATTRIBUTE_DIRECTORY) != 0;
    }
    if (!hasPlausibleUncShareSyntax(candidate)) {
        return false;
    }
    const auto& disp = tools3000::core::MainThreadDispatcher::instance();
    if (disp.isInitialized() && disp.isOwnerThread()) {
        return false;
    }

    ensureProbeWorkerStarted();
    auto attrs = std::make_shared<std::atomic<DWORD>>(INVALID_FILE_ATTRIBUTES);
    auto done = std::make_shared<std::atomic<bool>>(false);
    {
        std::lock_guard lock(m_probeMutex);
        if (!m_probeRunning.load(std::memory_order_acquire)) return false;
        m_probeRequest = UncProbeRequest{candidate, attrs, done};
    }
    m_probeCv.notify_one();

    {
        std::unique_lock lock(m_probeMutex);
        const bool completed = m_probeDoneCv.wait_for(
            lock, std::chrono::milliseconds(kUncProbeTimeoutMs),
            [&]() { return done->load(std::memory_order_acquire) || !m_probeRunning.load(std::memory_order_acquire); });
        if (!completed || !done->load(std::memory_order_acquire)) {
            if (m_probeWorker.joinable()) {
                HANDLE hThread = static_cast<HANDLE>(m_probeWorker.native_handle());
                if (hThread) CancelSynchronousIo(hThread);
            }
            LOG_WARN("DialogNavigator: UNC path attribute probe timed out (>200ms), skipping offline share");
            return false;
        }
    }
    const DWORD resultAttrs = attrs->load(std::memory_order_acquire);
    return resultAttrs != INVALID_FILE_ATTRIBUTES && (resultAttrs & FILE_ATTRIBUTE_DIRECTORY) != 0;
}

// ============================================================
// 零 UIA 轻量化目录读取（供 220ms 轮询与导航后实测验证使用）
// ============================================================
std::string DialogNavigator::getCurrentDialogFolderFast(HWND dialogHwnd, HWND& cachedToolbarHwnd) {
    if (!dialogHwnd || !IsWindow(dialogHwnd)) return "";

    if (cachedToolbarHwnd && IsWindow(cachedToolbarHwnd) && isVisibleNonEmptyRect(cachedToolbarHwnd)) {
        const std::wstring dir = extractExistingDirectory(getWindowTextWithTimeout(cachedToolbarHwnd));
        if (!dir.empty()) {
            return tools3000::core::WinUtils::wstringToUtf8(dir);
        }
    }

    struct ToolbarProbe {
        HWND foundHwnd{nullptr};
        std::wstring path;
    } probe{};

    EnumChildWindows(dialogHwnd, [](HWND child, LPARAM param) -> BOOL {
        wchar_t cls[32]{};
        if (GetClassNameW(child, cls, static_cast<int>(std::size(cls))) <= 0 ||
            wcscmp(cls, L"ToolbarWindow32") != 0 ||
            !isVisibleNonEmptyRect(child)) {
            return TRUE;
        }
        auto* p = reinterpret_cast<ToolbarProbe*>(param);
        const std::wstring dir = extractExistingDirectory(getWindowTextWithTimeout(child));
        if (!dir.empty()) {
            p->foundHwnd = child;
            p->path = dir;
            return FALSE;
        }
        return TRUE;
    }, reinterpret_cast<LPARAM>(&probe));

    if (!probe.path.empty()) {
        cachedToolbarHwnd = probe.foundHwnd;
        return tools3000::core::WinUtils::wstringToUtf8(probe.path);
    }
    return "";
}

// ============================================================
// UIAutomation 辅助读取（按需兜底）
// ============================================================
std::string DialogNavigator::uiaGetAddressBarPath(HWND dialogHwnd) {
    HWND dummyToolbar = nullptr;
    std::string fast = getCurrentDialogFolderFast(dialogHwnd, dummyToolbar);
    if (!fast.empty()) return fast;

    IUIAutomation* uia = getUIA();
    if (!uia) return "";

    ComPtr<IUIAutomationElement> dlgElem;
    if (FAILED(uia->ElementFromHandle(dialogHwnd, dlgElem.GetAddressOf())) || !dlgElem) return "";

    VARIANT addressId{};
    addressId.vt = VT_BSTR;
    addressId.bstrVal = SysAllocString(L"41477");
    ComPtr<IUIAutomationCondition> addressCondition;
    const HRESULT addressConditionResult = uia->CreatePropertyCondition(
        UIA_AutomationIdPropertyId, addressId, addressCondition.GetAddressOf());
    VariantClear(&addressId);
    if (SUCCEEDED(addressConditionResult) && addressCondition) {
        ComPtr<IUIAutomationElementArray> addressRoots;
        if (SUCCEEDED(dlgElem->FindAll(TreeScope_Descendants, addressCondition.Get(),
                                      addressRoots.GetAddressOf())) && addressRoots) {
            ComPtr<IUIAutomationCondition> trueCondition;
            if (SUCCEEDED(uia->CreateTrueCondition(trueCondition.GetAddressOf())) && trueCondition) {
                int rootCount = 0;
                addressRoots->get_Length(&rootCount);
                for (int rootIndex = 0; rootIndex < rootCount; ++rootIndex) {
                    ComPtr<IUIAutomationElement> addressRoot;
                    if (FAILED(addressRoots->GetElement(rootIndex, addressRoot.GetAddressOf())) ||
                        !addressRoot) continue;
                    BOOL offscreen = TRUE;
                    RECT bounds{};
                    if (FAILED(addressRoot->get_CurrentIsOffscreen(&offscreen)) || offscreen ||
                        FAILED(addressRoot->get_CurrentBoundingRectangle(&bounds)) ||
                        bounds.right <= bounds.left || bounds.bottom <= bounds.top) {
                        continue;
                    }

                    ComPtr<IUIAutomationElementArray> addressElements;
                    if (FAILED(addressRoot->FindAll(TreeScope_Subtree, trueCondition.Get(),
                                                    addressElements.GetAddressOf())) ||
                        !addressElements) continue;
                    int count = 0;
                    addressElements->get_Length(&count);
                    for (int index = 0; index < count; ++index) {
                        ComPtr<IUIAutomationElement> element;
                        if (FAILED(addressElements->GetElement(index, element.GetAddressOf())) ||
                            !element) continue;
                        BSTR rawName = nullptr;
                        if (FAILED(element->get_CurrentName(&rawName)) || !rawName) continue;
                        const std::wstring path = extractExistingDirectory(rawName);
                        SysFreeString(rawName);
                        if (!path.empty()) {
                            return tools3000::core::WinUtils::wstringToUtf8(path);
                        }
                    }
                }
            }
        }
    }

    RECT dlgRect{};
    if (GetWindowRect(dialogHwnd, &dlgRect)) {
        HWND visibleEdit = findSafeVisibleAddressEdit41477(dialogHwnd, dlgRect);
        if (visibleEdit) {
            const std::wstring path = extractExistingDirectory(getWindowTextWithTimeout(visibleEdit));
            if (!path.empty()) return tools3000::core::WinUtils::wstringToUtf8(path);
        }
    }
    return "";
}

std::string DialogNavigator::uiaGetFileNameText(HWND dialogHwnd, bool* controlFound) {
    if (controlFound) *controlFound = false;
    HWND editHwnd = findPathEditControl(dialogHwnd);
    if (editHwnd) {
        if (controlFound) *controlFound = true;
        const std::wstring text = getWindowTextWithTimeout(editHwnd);
        if (!text.empty()) return tools3000::core::WinUtils::wstringToUtf8(text);
    }

    IUIAutomation* uia = getUIA();
    if (!uia) return "";

    ComPtr<IUIAutomationElement> dlgElem;
    if (FAILED(uia->ElementFromHandle(dialogHwnd, dlgElem.GetAddressOf())) || !dlgElem) return "";

    VARIANT id{};
    id.vt = VT_BSTR;
    id.bstrVal = SysAllocString(L"1152");
    ComPtr<IUIAutomationCondition> idCondition;
    const HRESULT conditionResult = uia->CreatePropertyCondition(
        UIA_AutomationIdPropertyId, id, idCondition.GetAddressOf());
    VariantClear(&id);
    if (FAILED(conditionResult) || !idCondition) return "";

    ComPtr<IUIAutomationElement> fileNameElement;
    if (FAILED(dlgElem->FindFirst(TreeScope_Descendants, idCondition.Get(),
                                  fileNameElement.GetAddressOf())) || !fileNameElement) return "";
    if (controlFound) *controlFound = true;
    const std::wstring value = uiaGetValue(fileNameElement.Get());
    return value.empty() ? "" : tools3000::core::WinUtils::wstringToUtf8(value);
}

// ============================================================
// 异步后台 MTA 工作线程（导航单槽与普通异步队列物理分离）
// ============================================================
void DialogNavigator::startAsyncWorker() {
    std::lock_guard lock(m_asyncMutex);
    if (m_asyncRunning.load(std::memory_order_acquire)) {
        return;
    }
    m_asyncRunning.store(true, std::memory_order_release);
    if (m_asyncWorker.joinable()) {
        m_asyncWorker.join();
    }
    m_asyncWorker = std::thread([this]() noexcept {
        asyncWorkerLoop();
    });
}

void DialogNavigator::cancelPendingNavigation() {
    m_navCancelEpoch.fetch_add(1, std::memory_order_acq_rel);
    {
        std::lock_guard lock(m_asyncMutex);
        // 仅清空导航单槽，绝不误杀 m_generalQueue 中的收藏/固定工作区等普通任务
        m_pendingNavTask.reset();
    }
    m_asyncCv.notify_all();
}

void DialogNavigator::shutdownAsyncWorker() {
    m_asyncRunning.store(false, std::memory_order_release);
    m_navCancelEpoch.fetch_add(1, std::memory_order_acq_rel);
    {
        std::lock_guard lock(m_asyncMutex);
        m_pendingNavTask.reset();
        m_generalQueue.clear();
    }
    m_asyncCv.notify_all();
    if (m_asyncWorker.joinable() && m_asyncWorker.get_id() != std::this_thread::get_id()) {
        m_asyncWorker.join();
    }
    shutdownProbeWorker();
}

bool DialogNavigator::waitInterruptibleMs(uint32_t ms, uint64_t expectedNavEpoch) const {
    if (ms == 0) return true;
    const uint64_t startEpoch = m_navCancelEpoch.load(std::memory_order_acquire);
    std::unique_lock lock(m_asyncMutex);
    const bool interrupted = m_asyncCv.wait_for(
        lock,
        std::chrono::milliseconds(ms),
        [this, expectedNavEpoch, startEpoch]() {
            if (expectedNavEpoch != 0) {
                return !m_asyncRunning.load(std::memory_order_acquire) ||
                       m_navCancelEpoch.load(std::memory_order_acquire) != expectedNavEpoch;
            }
            return m_navCancelEpoch.load(std::memory_order_acquire) != startEpoch;
        });
    return !interrupted;
}

void DialogNavigator::postAsyncTask(std::function<void()> task) {
    if (!task) return;
    startAsyncWorker();
    {
        std::lock_guard lock(m_asyncMutex);
        if (!m_asyncRunning.load(std::memory_order_acquire)) return;
        if (m_generalQueue.size() >= 32) {
            m_generalQueue.pop_front();
        }
        m_generalQueue.push_back(std::move(task));
    }
    m_asyncCv.notify_one();
}

void DialogNavigator::navigateToFolderAsync(HWND dialogHwnd,
                                            const std::string& folderPath,
                                            std::function<void(bool)> onComplete) {
    if (!dialogHwnd || folderPath.empty()) {
        if (onComplete) onComplete(false);
        return;
    }
    startAsyncWorker();
    const uint64_t epoch = m_navCancelEpoch.fetch_add(1, std::memory_order_acq_rel) + 1;
    {
        std::lock_guard lock(m_asyncMutex);
        if (!m_asyncRunning.load(std::memory_order_acquire)) {
            if (onComplete) onComplete(false);
            return;
        }
        // 新导航仅覆盖单槽，绝不影响 m_generalQueue
        m_pendingNavTask = [this, dialogHwnd, folderPath, epoch, cb = std::move(onComplete)]() mutable {
            if (m_navCancelEpoch.load(std::memory_order_acquire) != epoch ||
                !m_asyncRunning.load(std::memory_order_acquire)) {
                if (cb) cb(false);
                return;
            }
            const bool ok = navigateToFolderInternal(dialogHwnd, folderPath, epoch);
            if (cb) cb(ok);
        };
    }
    m_asyncCv.notify_all();
}

void DialogNavigator::navigateToSearchResultAsync(HWND dialogHwnd,
                                                  const std::string& itemPath,
                                                  bool itemIsDirectory,
                                                  DialogMode mode,
                                                  bool ctrlPressed,
                                                  bool shiftPressed,
                                                  std::function<void(bool)> onComplete) {
    if (!dialogHwnd || itemPath.empty()) {
        if (onComplete) onComplete(false);
        return;
    }
    startAsyncWorker();
    const uint64_t epoch = m_navCancelEpoch.fetch_add(1, std::memory_order_acq_rel) + 1;
    {
        std::lock_guard lock(m_asyncMutex);
        if (!m_asyncRunning.load(std::memory_order_acquire)) {
            if (onComplete) onComplete(false);
            return;
        }
        m_pendingNavTask = [this, dialogHwnd, itemPath, itemIsDirectory, mode,
                            ctrlPressed, shiftPressed, epoch, cb = std::move(onComplete)]() mutable {
            if (m_navCancelEpoch.load(std::memory_order_acquire) != epoch ||
                !m_asyncRunning.load(std::memory_order_acquire)) {
                if (cb) cb(false);
                return;
            }
            const bool ok = navigateToSearchResultInternal(
                dialogHwnd, itemPath, itemIsDirectory, mode, ctrlPressed, shiftPressed, epoch);
            if (cb) cb(ok);
        };
    }
    m_asyncCv.notify_all();
}

void DialogNavigator::asyncWorkerLoop() {
    (void)getUIA();

    while (m_asyncRunning.load(std::memory_order_acquire)) {
        std::function<void()> task;
        {
            std::unique_lock lock(m_asyncMutex);
            m_asyncCv.wait(lock, [this]() {
                return !m_asyncRunning.load(std::memory_order_acquire) ||
                       m_pendingNavTask.has_value() ||
                       !m_generalQueue.empty();
            });
            if (!m_asyncRunning.load(std::memory_order_acquire)) break;
            if (m_pendingNavTask.has_value()) {
                task = std::move(*m_pendingNavTask);
                m_pendingNavTask.reset();
            } else if (!m_generalQueue.empty()) {
                task = std::move(m_generalQueue.front());
                m_generalQueue.pop_front();
            }
        }
        if (task) {
            try {
                task();
            } catch (const std::exception& ex) {
                LOG_WARN("DialogNavigator: async worker task exception: {}", ex.what());
            } catch (...) {
                LOG_WARN("DialogNavigator: async worker task unknown exception");
            }
        }
    }
}

// ============================================================
// Modern IFileDialog 毫秒级直达管道（零 500ms 自旋 / 零 F4 / 实测验证到位）
// ============================================================
bool DialogNavigator::navigateModernDialog(HWND dialogHwnd,
                                           const DialogControlSnapshot& snap,
                                           const std::wstring& wPath,
                                           uint64_t navEpoch) {
    RECT dialogRect{};
    if (!GetWindowRect(dialogHwnd, &dialogRect)) return false;

    HWND toolbarHwnd = snap.addressToolbarHwnd;
    const std::string beforeFolderUtf8 = getCurrentDialogFolderFast(dialogHwnd, toolbarHwnd);
    const std::wstring beforeFolderW = tools3000::core::WinUtils::utf8ToWstring(beforeFolderUtf8);
    if (!beforeFolderW.empty() && pathsEqualIgnoreCaseW(beforeFolderW, wPath)) {
        return true;
    }

    auto isCancelled = [&]() noexcept -> bool {
        return navEpoch != 0 &&
               m_navCancelEpoch.load(std::memory_order_acquire) != navEpoch;
    };

    auto submitAndVerifyEdit = [&](HWND targetEdit, int maxVerifySteps, std::wstring& outObserved) -> bool {
        if (!targetEdit || !IsWindow(targetEdit)) return false;
        LRESULT ignored = 0;
        bool textWritten = sendMessageWithTimeout(
            targetEdit, WM_SETTEXT, 0, reinterpret_cast<LPARAM>(wPath.c_str()),
            ignored, kCrossProcessTimeoutMs);

        if (!textWritten) {
            if (IUIAutomation* uia = getUIA()) {
                ComPtr<IUIAutomationElement> elem;
                ComPtr<IUIAutomationValuePattern> vp;
                if (SUCCEEDED(uia->ElementFromHandle(targetEdit, elem.GetAddressOf())) && elem &&
                    SUCCEEDED(elem->GetCurrentPatternAs(UIA_ValuePatternId, IID_IUIAutomationValuePattern,
                                                        reinterpret_cast<void**>(vp.GetAddressOf()))) && vp) {
                    BSTR bstrPath = SysAllocString(wPath.c_str());
                    textWritten = SUCCEEDED(vp->SetValue(bstrPath));
                    SysFreeString(bstrPath);
                }
            }
        }
        if (!textWritten) {
            return false;
        }

        const LPARAM keyDown = 1 | (static_cast<LPARAM>(MapVirtualKeyW(VK_RETURN, MAPVK_VK_TO_VSC)) << 16);
        const LPARAM keyUp = keyDown | (static_cast<LPARAM>(1) << 30) | (static_cast<LPARAM>(1) << 31);
        if (!sendMessageWithTimeout(targetEdit, WM_KEYDOWN, VK_RETURN, keyDown, ignored, kCrossProcessTimeoutMs)) {
            PostMessageW(targetEdit, WM_KEYDOWN, VK_RETURN, keyDown);
        }
        if (!sendMessageWithTimeout(targetEdit, WM_KEYUP, VK_RETURN, keyUp, ignored, kCrossProcessTimeoutMs)) {
            PostMessageW(targetEdit, WM_KEYUP, VK_RETURN, keyUp);
        }

        // 20ms 步进实测验证地址栏面包屑路径真实变更，绝不盲目返回假成功！
        for (int step = 0; step < maxVerifySteps; ++step) {
            if (!waitInterruptibleMs(20, navEpoch)) return false;
            if (!IsWindow(dialogHwnd)) {
                LOG_ERROR("DialogNavigator: modern dialog closed unexpectedly after address commit");
                return false;
            }
            const std::string curUtf8 = getCurrentDialogFolderFast(dialogHwnd, toolbarHwnd);
            if (!curUtf8.empty()) {
                outObserved = tools3000::core::WinUtils::utf8ToWstring(curUtf8);
                if (pathsEqualIgnoreCaseW(outObserved, wPath)) {
                    return true;
                }
                if (!IsWindowVisible(targetEdit) && !beforeFolderW.empty() &&
                    !pathsEqualIgnoreCaseW(outObserved, beforeFolderW)) {
                    return true;
                }
            }
            if (step == 4 && IsWindow(targetEdit) && IsWindowVisible(targetEdit)) {
                PostMessageW(targetEdit, WM_CHAR, VK_RETURN, keyDown);
            }
        }

        const std::string fallbackCur = uiaGetAddressBarPath(dialogHwnd);
        if (!fallbackCur.empty()) {
            const std::wstring fallbackW = tools3000::core::WinUtils::utf8ToWstring(fallbackCur);
            outObserved = fallbackW;
            if (pathsEqualIgnoreCaseW(fallbackW, wPath) ||
                (!IsWindowVisible(targetEdit) && !beforeFolderW.empty() &&
                 !pathsEqualIgnoreCaseW(fallbackW, beforeFolderW))) {
                return true;
            }
        }
        return false;
    };

    std::wstring lastObservedW;

    // 通道 1（零前台焦点依赖 / 0 SendInput / ~20ms 极速直达）：
    // Windows 10/11 现代 IFileDialog 的 Edit 41477 在面包屑态下即已存在于控件树中并通过全部几何校验，
    // 直接提交后由实测面包屑验证闭环兜底，兼容前后台与无头自动化会话。
    HWND editHwnd = findSafeAddressEdit41477(dialogHwnd, dialogRect, false);
    if (editHwnd && !isCancelled()) {
        if (submitAndVerifyEdit(editHwnd, 14, lastObservedW)) {
            LOG_INFO("DialogNavigator: modern address navigation verified (direct Edit 41477): {}",
                     tools3000::core::WinUtils::wstringToUtf8(wPath));
            return true;
        }
    }

    // 通道 2：若直接提交未触发面包屑变更或尚未创建 Edit 41477，显式激活可见 Edit 41477（静默点击面包屑 -> 回退 Ctrl+L / Alt+D）
    HWND visibleEdit = findSafeVisibleAddressEdit41477(dialogHwnd, dialogRect);
    if (!visibleEdit && !isCancelled() && activateAddressEditSilently(dialogHwnd, snap)) {
        for (int attempt = 0; attempt < 5 && !visibleEdit && IsWindow(dialogHwnd); ++attempt) {
            if (!waitInterruptibleMs(8, navEpoch)) return false;
            visibleEdit = findSafeVisibleAddressEdit41477(dialogHwnd, dialogRect);
        }
    }

    if (!visibleEdit && !isCancelled() && !snap.isInstallerOrWizard) {
        struct Activator { WORD modifier; WORD key; const char* name; };
        static constexpr Activator kSafeActivators[] = {
            {VK_CONTROL, 'L', "Ctrl+L"},
            {VK_MENU, 'D', "Alt+D"},
        };
        for (const auto& act : kSafeActivators) {
            if (!sendKeyChord(dialogHwnd, act.modifier, act.key, navEpoch)) {
                LOG_WARN("DialogNavigator: modern activator {} failed to send", act.name);
                continue;
            }
            for (int attempt = 0; attempt < 6 && !visibleEdit && IsWindow(dialogHwnd); ++attempt) {
                if (!waitInterruptibleMs(10, navEpoch)) return false;
                visibleEdit = findSafeVisibleAddressEdit41477(dialogHwnd, dialogRect);
            }
            if (visibleEdit) break;
        }
    }

    if (!visibleEdit || !IsWindow(visibleEdit) || !IsWindowVisible(visibleEdit)) {
        LOG_WARN("DialogNavigator: visible Edit 41477 not revealed, target={}, observed={}",
                 tools3000::core::WinUtils::wstringToUtf8(wPath),
                 tools3000::core::WinUtils::wstringToUtf8(lastObservedW));
        return false;
    }

    const bool verified = submitAndVerifyEdit(visibleEdit, 16, lastObservedW);
    if (verified) {
        LOG_INFO("DialogNavigator: modern address navigation verified (activated Edit 41477): {}",
                 tools3000::core::WinUtils::wstringToUtf8(wPath));
    } else {
        LOG_WARN("DialogNavigator: modern address navigation unverified after 320ms, target={}, observed={}",
                 tools3000::core::WinUtils::wstringToUtf8(wPath),
                 tools3000::core::WinUtils::wstringToUtf8(lastObservedW));
    }
    return verified;
}

// ============================================================
// LegacyExplorer (OFN_EXPLORER) / ClassicOpenFileName 可靠管道
// ============================================================
bool DialogNavigator::navigateLegacyDialog(HWND dialogHwnd,
                                           const DialogControlSnapshot& snap,
                                           const std::wstring& wPath,
                                           uint64_t navEpoch) {
    HWND editHwnd = snap.fileNameEditHwnd;
    if (!editHwnd || !IsWindow(editHwnd) || isEditReadOnly(editHwnd)) {
        editHwnd = findPathEditControl(dialogHwnd);
    }
    if (!editHwnd || !IsWindow(editHwnd)) {
        LOG_WARN("DialogNavigator: path edit control not found, hwnd=0x{:X}",
                 reinterpret_cast<uintptr_t>(dialogHwnd));
        return false;
    }

    // 安装向导绝不发送确认命令/回车，仅安全写入文本
    if (snap.isInstallerOrWizard) {
        LRESULT ignored = 0;
        if (!sendMessageWithTimeout(editHwnd, WM_SETTEXT, 0,
                                    reinterpret_cast<LPARAM>(wPath.c_str()), ignored)) {
            return false;
        }
        HWND parent = GetParent(editHwnd);
        if (parent) {
            sendMessageWithTimeout(parent, WM_COMMAND,
                                   MAKEWPARAM(GetDlgCtrlID(editHwnd), EN_CHANGE),
                                   reinterpret_cast<LPARAM>(editHwnd), ignored);
        }
        LOG_INFO("DialogNavigator: installer directory updated safely via WM_SETTEXT without commit: {}",
                 tools3000::core::WinUtils::wstringToUtf8(wPath));
        return true;
    }

    std::wstring wPathDir = wPath;
    if (wPathDir.back() != L'\\' && wPathDir.back() != L'/') {
        wPathDir.push_back(L'\\');
    }

    // 使用跨进程有效 WM_GETTEXTLENGTH + WM_GETTEXT 完整备份原有保存文件名
    LRESULT rawOldLen = 0;
    size_t reserveChars = MAX_PATH * 2;
    if (sendMessageWithTimeout(editHwnd, WM_GETTEXTLENGTH, 0, 0, rawOldLen) && rawOldLen >= 0) {
        reserveChars = (std::min)(static_cast<size_t>(rawOldLen) + 2, static_cast<size_t>(32768));
    }
    const std::wstring oldText =
        (rawOldLen > 0) ? getWindowTextWithTimeout(editHwnd, reserveChars) : std::wstring{};

    HWND toolbarHwnd = snap.addressToolbarHwnd;
    const std::string beforeFolderUtf8 = getCurrentDialogFolderFast(dialogHwnd, toolbarHwnd);

    LRESULT ignored = 0;
    if (!sendMessageWithTimeout(editHwnd, WM_SETTEXT, 0,
                                reinterpret_cast<LPARAM>(wPathDir.c_str()), ignored)) {
        return false;
    }
    HWND editParent = GetParent(editHwnd);
    if (editParent) {
        sendMessageWithTimeout(editParent, WM_COMMAND,
                               MAKEWPARAM(GetDlgCtrlID(editHwnd), EN_CHANGE),
                               reinterpret_cast<LPARAM>(editHwnd), ignored);
    }

    // 优先通过标准对话框命令路由 WM_COMMAND(IDOK, BN_CLICKED) 触发目录切换（不绕过 IsDialogMessage）
    HWND okBtn = snap.okButtonHwnd ? snap.okButtonHwnd : GetDlgItem(dialogHwnd, IDOK);
    const bool postedCmd = PostMessageW(
        dialogHwnd, WM_COMMAND, MAKEWPARAM(IDOK, BN_CLICKED), reinterpret_cast<LPARAM>(okBtn)) != FALSE;
    if (!postedCmd && okBtn && IsWindow(okBtn)) {
        PostMessageW(okBtn, BM_CLICK, 0, 0);
    }

    // 20ms 步进等待目录切换真正生效后再恢复 oldText，彻底消灭 80ms 竞态！
    bool switched = false;
    bool sentReturnFallback = false;
    for (int step = 0; step < 12; ++step) {
        if (!waitInterruptibleMs(20, navEpoch)) return false;
        if (!IsWindow(dialogHwnd)) {
            LOG_ERROR("DialogNavigator: legacy dialog closed unexpectedly during navigation");
            return false;
        }
        const std::wstring curEditText = getWindowTextWithTimeout(editHwnd, reserveChars + wPathDir.size() + 16);
        const std::string curFolder = getCurrentDialogFolderFast(dialogHwnd, toolbarHwnd);
        if (_wcsicmp(curEditText.c_str(), wPathDir.c_str()) != 0 ||
            (!curFolder.empty() && curFolder != beforeFolderUtf8)) {
            switched = true;
            break;
        }
        if (step == 4 && !sentReturnFallback) {
            sentReturnFallback = true;
            const LPARAM keyDown = 1 | (static_cast<LPARAM>(MapVirtualKeyW(VK_RETURN, MAPVK_VK_TO_VSC)) << 16);
            const LPARAM keyUp = keyDown | (static_cast<LPARAM>(1) << 30) | (static_cast<LPARAM>(1) << 31);
            PostMessageW(editHwnd, WM_KEYDOWN, VK_RETURN, keyDown);
            PostMessageW(editHwnd, WM_KEYUP, VK_RETURN, keyUp);
        }
    }

    if (!IsWindow(dialogHwnd)) {
        return false;
    }

    // 仅在对话框仍存活时安全恢复原文件名（另存为场景完好无损）
    if (IsWindow(editHwnd)) {
        sendMessageWithTimeout(editHwnd, WM_SETTEXT, 0,
                               reinterpret_cast<LPARAM>(oldText.c_str()), ignored);
        if (editParent && IsWindow(editParent)) {
            sendMessageWithTimeout(editParent, WM_COMMAND,
                                   MAKEWPARAM(GetDlgCtrlID(editHwnd), EN_CHANGE),
                                   reinterpret_cast<LPARAM>(editHwnd), ignored);
        }
    }

    LOG_INFO("DialogNavigator: legacy navigation finished: path={}, switched={}",
             tools3000::core::WinUtils::wstringToUtf8(wPath), switched);
    return switched || IsWindow(dialogHwnd);
}

// ============================================================
// getCurrentDialogFolder / getSelectedPath
// ============================================================
std::string DialogNavigator::getCurrentDialogFolder(HWND dialogHwnd) {
    if (!dialogHwnd || !IsWindow(dialogHwnd)) return "";

    const DialogControlSnapshot snap = inspectDialog(dialogHwnd, true);
    if (snap.architecture == DialogArchitecture::ModernIFileDialog) {
        HWND tb = snap.addressToolbarHwnd;
        std::string fast = getCurrentDialogFolderFast(dialogHwnd, tb);
        if (!fast.empty()) return fast;
        std::string uiaPath = uiaGetAddressBarPath(dialogHwnd);
        if (!uiaPath.empty()) return uiaPath;
    }

    if (snap.architecture == DialogArchitecture::BrowseForFolder) {
        HWND edit = snap.fileNameEditHwnd ? snap.fileNameEditHwnd : findPathEditControl(dialogHwnd);
        if (edit) {
            const std::wstring text = getWindowTextWithTimeout(edit, MAX_PATH * 2);
            if (!text.empty() && isDirectoryFastNonBlocking(text)) {
                return tools3000::core::WinUtils::wstringToUtf8(text);
            }
        }
    }

    // 严禁发送 CDM_GETFOLDERPATH (跨进程裸指针崩溃)；走安全带超时的 WM_GETTEXT 扫描
    std::string foundPath;
    EnumChildWindows(dialogHwnd, [](HWND hwnd, LPARAM lParam) -> BOOL {
        auto* outStr = reinterpret_cast<std::string*>(lParam);
        const std::wstring text = getWindowTextWithTimeout(hwnd, MAX_PATH * 2);
        const std::wstring dir = extractExistingDirectory(text);
        if (!dir.empty()) {
            *outStr = tools3000::core::WinUtils::wstringToUtf8(dir);
            return FALSE;
        }
        return TRUE;
    }, reinterpret_cast<LPARAM>(&foundPath));
    return foundPath;
}

std::string DialogNavigator::getSelectedPath(HWND dialogHwnd) {
    if (!dialogHwnd || !IsWindow(dialogHwnd)) return "";

    const DialogControlSnapshot snap = inspectDialog(dialogHwnd, true);
    if (snap.architecture == DialogArchitecture::ModernIFileDialog) {
        bool fileNameControlFound = false;
        const std::string fileName = uiaGetFileNameText(dialogHwnd, &fileNameControlFound);
        if (!fileName.empty()) {
            const std::wstring wFileName = tools3000::core::WinUtils::utf8ToWstring(fileName);
            if (isPathExistingFastNonBlocking(wFileName)) {
                return fileName;
            }
            const std::string curFolder = getCurrentDialogFolder(dialogHwnd);
            if (!curFolder.empty()) {
                const std::filesystem::path cand =
                    tools3000::core::WinUtils::utf8ToPath(curFolder) / wFileName;
                if (isPathExistingFastNonBlocking(cand.native())) {
                    return tools3000::core::WinUtils::pathToUtf8(cand);
                }
            }
        }
        if (!fileNameControlFound || fileName.empty()) {
            const std::string currentFolder = getCurrentDialogFolder(dialogHwnd);
            const std::string selectedChild = uiaGetSelectedShellChild(dialogHwnd, currentFolder);
            if (!selectedChild.empty()) return selectedChild;
        }
        return "";
    }

    HWND editHwnd = snap.fileNameEditHwnd ? snap.fileNameEditHwnd : findPathEditControl(dialogHwnd);
    if (editHwnd && IsWindow(editHwnd)) {
        const std::wstring editStr = getWindowTextWithTimeout(editHwnd, MAX_PATH * 2);
        if (!editStr.empty()) {
            if (isPathExistingFastNonBlocking(editStr)) {
                return tools3000::core::WinUtils::wstringToUtf8(editStr);
            }
            const std::string curFolder = getCurrentDialogFolder(dialogHwnd);
            if (!curFolder.empty()) {
                const std::filesystem::path cand =
                    tools3000::core::WinUtils::utf8ToPath(curFolder) / editStr;
                if (isPathExistingFastNonBlocking(cand.native())) {
                    return tools3000::core::WinUtils::pathToUtf8(cand);
                }
            }
        }
    }
    return "";
}

// ============================================================
// navigateToFolder / navigateToSearchResult
// ============================================================
bool DialogNavigator::navigateToFolder(HWND dialogHwnd, const std::string& folderPath) {
    return navigateToFolderInternal(dialogHwnd, folderPath, 0);
}

bool DialogNavigator::navigateToFolderInternal(HWND dialogHwnd,
                                               const std::string& folderPath,
                                               uint64_t navEpoch) {
    if (!dialogHwnd || !IsWindow(dialogHwnd) || folderPath.empty()) return false;

    const std::wstring wPath = tools3000::core::WinUtils::utf8ToWstring(folderPath);
    if (wPath.empty()) return false;
    if (!verifyNavigableDirectoryBounded(wPath)) {
        LOG_WARN("DialogNavigator: reject navigating to non-directory or non-existent path: {}", folderPath);
        return false;
    }

    const DialogControlSnapshot snap = inspectDialog(dialogHwnd, true);
    LOG_INFO("DialogNavigator: navigating dialog hwnd=0x{:X}, arch={}, mode={}, path={}",
             reinterpret_cast<uintptr_t>(dialogHwnd),
             static_cast<int>(snap.architecture),
             static_cast<int>(snap.mode),
             folderPath);

    if (snap.architecture == DialogArchitecture::ModernIFileDialog) {
        return navigateModernDialog(dialogHwnd, snap, wPath, navEpoch);
    }

    if (snap.architecture == DialogArchitecture::BrowseForFolder) {
        HWND editHwnd = snap.fileNameEditHwnd ? snap.fileNameEditHwnd : findPathEditControl(dialogHwnd);
        if (!editHwnd) {
            LOG_WARN("DialogNavigator: BrowseForFolder has no edit control, skipping: hwnd=0x{:X}",
                     reinterpret_cast<uintptr_t>(dialogHwnd));
            return false;
        }
        LRESULT ignored = 0;
        if (!sendMessageWithTimeout(editHwnd, WM_SETTEXT, 0,
                                    reinterpret_cast<LPARAM>(wPath.c_str()), ignored)) {
            return false;
        }
        HWND parent = GetParent(editHwnd);
        if (parent) {
            sendMessageWithTimeout(parent, WM_COMMAND,
                                   MAKEWPARAM(GetDlgCtrlID(editHwnd), EN_CHANGE),
                                   reinterpret_cast<LPARAM>(editHwnd), ignored);
        }
        LOG_INFO("DialogNavigator: BrowseForFolder updated safely via WM_SETTEXT: {}", folderPath);
        return true;
    }

    return navigateLegacyDialog(dialogHwnd, snap, wPath, navEpoch);
}

bool DialogNavigator::navigateToSearchResult(HWND dialogHwnd,
                                             const std::string& itemPath,
                                             bool itemIsDirectory,
                                             DialogMode mode,
                                             bool ctrlPressed,
                                             bool shiftPressed) {
    return navigateToSearchResultInternal(
        dialogHwnd, itemPath, itemIsDirectory, mode, ctrlPressed, shiftPressed, 0);
}

bool DialogNavigator::navigateToSearchResultInternal(HWND dialogHwnd,
                                                     const std::string& itemPath,
                                                     bool itemIsDirectory,
                                                     DialogMode mode,
                                                     bool ctrlPressed,
                                                     bool shiftPressed,
                                                     uint64_t navEpoch) {
    if (!dialogHwnd || !IsWindow(dialogHwnd) || itemPath.empty()) return false;

    const std::filesystem::path fsPath = tools3000::core::WinUtils::utf8ToPath(itemPath).lexically_normal();
    if (fsPath.empty()) return false;

    // 如果未传明确模式，实时重检一次（无法确认仍为 Unknown -> 保守安全降级）
    DialogMode effectiveMode = mode;
    if (effectiveMode == DialogMode::Unknown) {
        effectiveMode = inspectDialog(dialogHwnd, true).mode;
    }

    const SearchActionDecision decision = decideInlineSearchAction(
        effectiveMode, itemIsDirectory, ctrlPressed, shiftPressed);

    const std::filesystem::path targetDir = decision.useParentDirectory
        ? fsPath.parent_path()
        : fsPath;
    if (targetDir.empty()) return false;

    const std::string targetDirUtf8 = tools3000::core::WinUtils::pathToUtf8(targetDir);
    const bool dirOk = navigateToFolderInternal(dialogHwnd, targetDirUtf8, navEpoch);
    if (!dirOk || !IsWindow(dialogHwnd)) {
        return dirOk;
    }

    // 仅跳目录模式（目录、Ctrl+Enter、SaveFile、PickFolder、Unknown）到此安全结束！
    if (decision.kind == SearchActionKind::NavigateFolderOnly) {
        return true;
    }

    // OpenFile 模式下选中文件：在确认父目录到位后，将文件名填入底部 Edit 1152
    const std::wstring fileNameW = fsPath.filename().wstring();
    if (fileNameW.empty()) return true;

    HWND fileEdit = findPathEditControl(dialogHwnd);
    if (!fileEdit || !IsWindow(fileEdit) || GetDlgCtrlID(fileEdit) == 41477) {
        LOG_WARN("DialogNavigator: OpenFile filename edit control not found after parent dir navigation");
        return true;
    }

    LRESULT ignored = 0;
    if (!sendMessageWithTimeout(fileEdit, WM_SETTEXT, 0,
                                reinterpret_cast<LPARAM>(fileNameW.c_str()), ignored)) {
        return true;
    }
    HWND editParent = GetParent(fileEdit);
    if (editParent && IsWindow(editParent)) {
        sendMessageWithTimeout(editParent, WM_COMMAND,
                               MAKEWPARAM(GetDlgCtrlID(fileEdit), EN_CHANGE),
                               reinterpret_cast<LPARAM>(fileEdit), ignored);
    }

    // 仅当 Shift+Enter 且确认为 OpenFile 时才触发确认打开；普通 Enter 绝不自动提交关闭！
    if (decision.kind == SearchActionKind::NavigateFillAndConfirmOpen) {
        if (!waitInterruptibleMs(30, navEpoch) || !IsWindow(dialogHwnd)) return true;
        HWND okBtn = findOkButtonControl(dialogHwnd);
        if (okBtn && IsWindow(okBtn)) {
            PostMessageW(dialogHwnd, WM_COMMAND, MAKEWPARAM(IDOK, BN_CLICKED),
                         reinterpret_cast<LPARAM>(okBtn));
        } else {
            PostMessageW(dialogHwnd, WM_COMMAND, MAKEWPARAM(IDOK, BN_CLICKED), 0);
        }
    }

    LOG_INFO("DialogNavigator: OpenFile search result applied: file={}, autoConfirm={}",
             tools3000::core::WinUtils::wstringToUtf8(fileNameW),
             decision.kind == SearchActionKind::NavigateFillAndConfirmOpen);
    return true;
}

}  // namespace tools3000::dialog
