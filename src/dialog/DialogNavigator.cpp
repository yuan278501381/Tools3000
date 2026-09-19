/**
 * Tools3000 - High Performance Windows Productivity Suite
 *
 * Copyright (c) 2026 Yy1 (GitHub yuan278501381) <https://github.com/yuan278501381> & Tools3000 contributors
 *
 * Licensed under the MIT License.
 */

#include "DialogNavigator.h"
#include "core/logger/Logger.h"
#include "core/utils/WinUtils.h"

#include <commdlg.h>
#include <dlgs.h>
#include <shlobj.h>
#include <shlwapi.h>
#include <uiautomation.h>
#include <algorithm>
#include <vector>
#include <thread>
#include <chrono>
#include <cwctype>
#include <filesystem>
#include <string_view>
#include <wrl/client.h>

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

// ============================================================
// 内部工具命名空间
// ============================================================
namespace {

bool sendMessageWithTimeout(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam,
                            LRESULT& result, UINT timeoutMs = 250) {
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

std::wstring getWindowTextWithTimeout(HWND hwnd, size_t maxChars = MAX_PATH * 2) {
    if (!hwnd || maxChars < 2) return {};
    std::wstring value(maxChars, L'\0');
    LRESULT copied = 0;
    if (!sendMessageWithTimeout(hwnd, WM_GETTEXT, static_cast<WPARAM>(maxChars),
                                reinterpret_cast<LPARAM>(value.data()), copied)) {
        return {};
    }
    if (copied <= 0) return {};
    value.resize(std::min<size_t>(static_cast<size_t>(copied), maxChars - 1));
    return value;
}

bool isVisibleAddressControl(HWND hwnd) {
    if (!hwnd || !IsWindowVisible(hwnd)) return false;
    RECT bounds{};
    return GetWindowRect(hwnd, &bounds) && bounds.right > bounds.left &&
           bounds.bottom > bounds.top;
}

bool isDirectionalMark(wchar_t value) {
    return value == 0x200E || value == 0x200F ||
           (value >= 0x202A && value <= 0x202E) ||
           (value >= 0x2066 && value <= 0x2069);
}

std::wstring sanitizeShellText(std::wstring value) {
    value.erase(std::remove_if(value.begin(), value.end(), isDirectionalMark), value.end());
    while (!value.empty() && iswspace(value.back())) value.pop_back();
    return value;
}

std::wstring extractExistingDirectory(const std::wstring& shellText) {
    const std::wstring text = sanitizeShellText(shellText);
    auto isDirectory = [](const std::wstring& candidate) {
        const DWORD attributes = GetFileAttributesW(candidate.c_str());
        return attributes != INVALID_FILE_ATTRIBUTES &&
               (attributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
    };

    for (size_t index = 0; index + 2 < text.size(); ++index) {
        const bool driveRoot = iswalpha(text[index]) && text[index + 1] == L':' &&
                               (text[index + 2] == L'\\' || text[index + 2] == L'/');
        const bool uncRoot = text[index] == L'\\' && text[index + 1] == L'\\' &&
                             index + 3 < text.size();
        if (!driveRoot && !uncRoot) continue;
        std::wstring candidate = text.substr(index);
        if (isDirectory(candidate)) return candidate;
    }
    return {};
}

bool isTargetForeground(HWND dialogHwnd) {
    const HWND foreground = GetForegroundWindow();
    return foreground == dialogHwnd ||
           (foreground && GetAncestor(foreground, GA_ROOT) == dialogHwnd);
}

bool ensureTargetForeground(HWND dialogHwnd) {
    if (isTargetForeground(dialogHwnd)) return true;
    SetForegroundWindow(dialogHwnd);
    for (int retry = 0; retry < 10; ++retry) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
        if (isTargetForeground(dialogHwnd)) return true;
    }
    return false;
}

bool sendKeyChord(HWND dialogHwnd, WORD modifier, WORD key) {
    if (!ensureTargetForeground(dialogHwnd)) return false;
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
    UINT sent = SendInput(count, input, sizeof(INPUT));
    if (sent < count && modifier != 0) {
        // 防御性安全释放修饰键，防止因前台切换导致修饰键卡在按下态。
        // 若为 Alt 键，先注入中立 VK_F24 脉冲，杜绝触发 Explorer 的 SC_KEYMENU 菜单模态并锁死文件拖拽
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

// 每个调用线程拥有独立的 UI Automation 对象和匹配的 COM apartment
// 生命周期。RPC_E_CHANGED_MODE 表示线程已经由宿主初始化为另一种
// apartment；此时可以沿用既有 apartment，但不能替宿主 CoUninitialize。
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
        // Apartment-bound interfaces must be released before COM is balanced.
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

// 安全读取 UIA ValuePattern 值
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

//// 枚举子控件上下文结构体
struct EnumChildContext {
    HWND editHwnd{nullptr};
    HWND standardFileEditHwnd{nullptr};
    HWND comboBoxHwnd{nullptr};
    HWND addressBandHwnd{nullptr};
    HWND shellViewHwnd{nullptr};
    HWND namespaceTreeHwnd{nullptr};
    HWND treeViewHwnd{nullptr};
    HWND okButtonHwnd{nullptr};
    HWND cancelButtonHwnd{nullptr};
    HWND backButtonHwnd{nullptr};
    bool hasTabControl{false};
    bool hasApplyButton{false};
    bool hasProgressBar{false};
    bool hasAnimation{false};
    bool hasProgressText{false};
    bool hasDefView{false};
    bool hasDirectUI{false};
    bool hasBreadcrumb{false};
    int totalControls{0};
};

BOOL CALLBACK EnumFileDialogChildren(HWND hwnd, LPARAM lParam) {
    auto* ctx = reinterpret_cast<EnumChildContext*>(lParam);
    if (!ctx) return FALSE;

    ctx->totalControls++;
    wchar_t className[64] = {0};
    GetClassNameW(hwnd, className, 64);
    int ctrlId = GetDlgCtrlID(hwnd);

    if (wcscmp(className, L"SHELLDLL_DefView") == 0) {
        ctx->hasDefView = true;
        if (!ctx->shellViewHwnd) ctx->shellViewHwnd = hwnd;
    } else if (wcscmp(className, L"DirectUIHWND") == 0 || wcscmp(className, L"DUIViewWndClassName") == 0) {
        ctx->hasDirectUI = true;
    } else if (wcscmp(className, L"NamespaceTreeControl") == 0) {
        ctx->namespaceTreeHwnd = hwnd;
    } else if (wcscmp(className, L"SysTreeView32") == 0) {
        ctx->treeViewHwnd = hwnd;
    } else if (wcscmp(className, L"SysTabControl32") == 0) {
        ctx->hasTabControl = true;
    } else if (wcscmp(className, L"msctls_progress32") == 0) {
        ctx->hasProgressBar = true;
    } else if (wcscmp(className, L"SysAnimate32") == 0) {
        ctx->hasAnimation = true;
    } else if (wcscmp(className, L"ComboBoxEx32") == 0 || wcscmp(className, L"ComboBox") == 0) {
        ctx->comboBoxHwnd = hwnd;
    } else if (wcscmp(className, L"Edit") == 0) {
        if (ctrlId == 0x047C || ctrlId == 1152) {
            ctx->standardFileEditHwnd = hwnd;
        }
        if (!ctx->editHwnd) {
            ctx->editHwnd = hwnd;
        }
    } else if (wcscmp(className, L"ToolbarWindow32") == 0 || wcscmp(className, L"Breadcrumb Parent") == 0) {
        ctx->hasBreadcrumb = true;
        HWND parent = GetParent(hwnd);
        if (parent) {
            wchar_t pClass[64] = {0};
            GetClassNameW(parent, pClass, 64);
            if (wcsstr(pClass, L"Address") || wcsstr(pClass, L"Breadcrumb") || wcsstr(pClass, L"ReBar")) {
                ctx->addressBandHwnd = hwnd;
            }
        }
    } else if (wcscmp(className, L"Button") == 0) {
        if (ctrlId == 0x3021 || ctrlId == 12321) {
            ctx->hasApplyButton = true;
        } else if (ctrlId == 0x3023 || ctrlId == 12323 || ctrlId == 1002 || ctrlId == 1028 || ctrlId == 1044) {
            ctx->backButtonHwnd = hwnd;
        } else if (ctrlId == IDOK || ctrlId == 1 || ctrlId == 0x0400) {
            ctx->okButtonHwnd = hwnd;
        } else if (ctrlId == IDCANCEL || ctrlId == 2) {
            ctx->cancelButtonHwnd = hwnd;
        }

        wchar_t btnText[64] = {0};
        if (GetWindowTextW(hwnd, btnText, 64) > 0) {
            std::wstring_view bv(btnText);
            if (bv.find(L"上一步") != std::wstring_view::npos ||
                bv.find(L"Back") != std::wstring_view::npos ||
                bv.find(L"back") != std::wstring_view::npos ||
                bv.find(L"<") != std::wstring_view::npos ||
                bv.find(L"戻る") != std::wstring_view::npos ||
                bv.find(L"Zurück") != std::wstring_view::npos ||
                bv.find(L"Précédent") != std::wstring_view::npos) {
                ctx->backButtonHwnd = hwnd;
            }
        }
    } else if (wcscmp(className, L"Static") == 0) {
        wchar_t text[128] = {0};
        if (GetWindowTextW(hwnd, text, 128) > 0) {
            std::wstring_view sv(text);
            if (sv.find(L"正在复制") != std::wstring_view::npos ||
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
                sv.find(L"Extracting") != std::wstring_view::npos) {
                ctx->hasProgressText = true;
            }
        }
    }
    return TRUE;
}

} // namespace

// ============================================================
// isProgressOrTransferDialog — 文件传输/复制/移动进度弹窗探测
// ============================================================
bool DialogNavigator::isProgressOrTransferDialog(HWND hwnd) {
    if (!hwnd || !IsWindow(hwnd)) return false;

    // 1. 窗口类名排查：资源管理器文件操作专用类名 OperationStatusWindow
    wchar_t className[64] = {0};
    if (GetClassNameW(hwnd, className, 64) > 0) {
        if (_wcsicmp(className, L"OperationStatusWindow") == 0) {
            return true;
        }
    }

    // 2. 根窗口类名排查
    HWND root = GetAncestor(hwnd, GA_ROOT);
    if (root && root != hwnd) {
        wchar_t rootClass[64] = {0};
        if (GetClassNameW(root, rootClass, 64) > 0 &&
            _wcsicmp(rootClass, L"OperationStatusWindow") == 0) {
            return true;
        }
    }

    // 3. 标题排查：多语言文件传输/操作进度关键词
    wchar_t title[256] = {0};
    GetWindowTextW(hwnd, title, 256);
    std::wstring_view tv(title);

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
        if (tv.find(keyword) != std::wstring_view::npos) {
            return true;
        }
    }

    // 4. 子控件特征排查：是否包含进度条或动画控件或进度文本
    bool hasProgressFeature = false;
    EnumChildWindows(hwnd, [](HWND child, LPARAM lParam) -> BOOL {
        wchar_t childClass[64] = {0};
        GetClassNameW(child, childClass, 64);
        if (_wcsicmp(childClass, L"msctls_progress32") == 0 ||
            _wcsicmp(childClass, L"SysAnimate32") == 0) {
            *reinterpret_cast<bool*>(lParam) = true;
            return FALSE;
        }

        wchar_t text[128] = {0};
        if (GetWindowTextW(child, text, 128) > 0) {
            std::wstring_view sv(text);
            if (sv.find(L"正在复制") != std::wstring_view::npos ||
                sv.find(L"正在移动") != std::wstring_view::npos ||
                sv.find(L"正在删除") != std::wstring_view::npos ||
                sv.find(L"正在传输") != std::wstring_view::npos ||
                sv.find(L"正在解压") != std::wstring_view::npos ||
                sv.find(L"Copying") != std::wstring_view::npos ||
                sv.find(L"Moving") != std::wstring_view::npos ||
                sv.find(L"Deleting") != std::wstring_view::npos ||
                sv.find(L"Transferring") != std::wstring_view::npos ||
                sv.find(L"Extracting") != std::wstring_view::npos) {
                *reinterpret_cast<bool*>(lParam) = true;
                return FALSE;
            }
        }
        return TRUE;
    }, reinterpret_cast<LPARAM>(&hasProgressFeature));

    return hasProgressFeature;
}

// ============================================================
// isInstallerProcess — 安装程序进程安全识别
// ============================================================
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

// ============================================================
// isInstallerOrWizard — 安装程序向导深度多维识别
// ============================================================
bool DialogNavigator::isInstallerOrWizard(HWND dialogHwnd) {
    if (!dialogHwnd || !IsWindow(dialogHwnd)) return false;

    // 1. 检查目标窗口所属进程及顶层 Owner 进程
    std::string processName = tools3000::core::WinUtils::getProcessNameFromWindow(dialogHwnd);
    if (isInstallerProcess(processName)) {
        return true;
    }

    HWND rootOwner = GetAncestor(dialogHwnd, GA_ROOTOWNER);
    if (rootOwner && rootOwner != dialogHwnd) {
        std::string ownerProc = tools3000::core::WinUtils::getProcessNameFromWindow(rootOwner);
        if (isInstallerProcess(ownerProc)) {
            return true;
        }
    }

    // 2. 检查窗口类名（Inno Setup, WiX, InstallShield, NSIS 等专有向导窗体类）
    auto checkClassName = [](HWND hwnd) -> bool {
        wchar_t cls[64] = {0};
        if (GetClassNameW(hwnd, cls, 64) > 0) {
            if (wcsstr(cls, L"TSetupForm") ||
                wcsstr(cls, L"TWizardForm") ||
                wcsstr(cls, L"TFolderTreeView") ||
                wcsstr(cls, L"TNewNotebook") ||
                wcsstr(cls, L"WixBurn") ||
                wcsstr(cls, L"InstallShield") ||
                wcsstr(cls, L"MsiDialogCloseClass")) {
                return true;
            }
        }
        return false;
    };

    if (checkClassName(dialogHwnd) || (rootOwner && checkClassName(rootOwner))) {
        return true;
    }

    // 3. 检查窗口标题与顶层 Owner 标题（安装向导多语言关键词）
    auto checkTitle = [](HWND hwnd) -> bool {
        wchar_t title[256] = {0};
        if (GetWindowTextW(hwnd, title, 256) > 0) {
            std::wstring_view tv(title);
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
                if (tv.find(kw) != std::wstring_view::npos) {
                    return true;
                }
            }
        }
        return false;
    };

    if (checkTitle(dialogHwnd) || (rootOwner && checkTitle(rootOwner))) {
        return true;
    }

    // 4. 检查子控件：是否具备向导标志性按钮（< Back / 上一步 / ID 0x3023 / 12323 / 1002 / 1028 / 1044）
    bool hasWizardControl = false;
    EnumChildWindows(dialogHwnd, [](HWND child, LPARAM lParam) -> BOOL {
        wchar_t cls[64] = {0};
        GetClassNameW(child, cls, 64);
        int ctrlId = GetDlgCtrlID(child);

        if (wcscmp(cls, L"Button") == 0) {
            if (ctrlId == 0x3023 || ctrlId == 12323 || ctrlId == 1002 || ctrlId == 1028 || ctrlId == 1044) {
                *reinterpret_cast<bool*>(lParam) = true;
                return FALSE;
            }
            wchar_t text[64] = {0};
            if (GetWindowTextW(child, text, 64) > 0) {
                std::wstring_view sv(text);
                if (sv.find(L"上一步") != std::wstring_view::npos ||
                    sv.find(L"Back") != std::wstring_view::npos ||
                    sv.find(L"back") != std::wstring_view::npos ||
                    sv.find(L"<") != std::wstring_view::npos ||
                    sv.find(L"戻る") != std::wstring_view::npos ||
                    sv.find(L"Zurück") != std::wstring_view::npos ||
                    sv.find(L"Précédent") != std::wstring_view::npos ||
                    sv.find(L"Назад") != std::wstring_view::npos) {
                    *reinterpret_cast<bool*>(lParam) = true;
                    return FALSE;
                }
            }
        }
        return TRUE;
    }, reinterpret_cast<LPARAM>(&hasWizardControl));

    if (hasWizardControl) {
        return true;
    }

    return false;
}

// ============================================================
// 对话框类型检测：Modern(IFileOpenDialog) vs Legacy(OPENFILENAME) vs FolderPicker
// ============================================================
DialogType DialogNavigator::detectDialogType(HWND hwnd) {
    if (!hwnd || !IsWindow(hwnd)) return DialogType::Unknown;

    EnumChildContext ctx;
    EnumChildWindows(hwnd, EnumFileDialogChildren, reinterpret_cast<LPARAM>(&ctx));

    // 如果包含向导后退按钮，则绝非标准文件对话框
    if (ctx.backButtonHwnd) {
        return DialogType::Unknown;
    }

    // Modern 对话框拥有 DefView 架构
    if (ctx.hasDefView) {
        return DialogType::Modern;
    }
    // FolderPicker: 具备树形控件且具备确认与取消按钮，控件数在合理范围内
    if ((ctx.namespaceTreeHwnd || ctx.treeViewHwnd) && ctx.okButtonHwnd && ctx.cancelButtonHwnd && ctx.totalControls <= 18) {
        return DialogType::FolderPicker;
    }
    // Legacy: 具备标准文件名输入控件
    if (ctx.standardFileEditHwnd || (ctx.comboBoxHwnd && ctx.okButtonHwnd)) {
        return DialogType::Legacy;
    }
    if (ctx.hasDirectUI && ctx.hasBreadcrumb && ctx.okButtonHwnd) {
        return DialogType::Modern;
    }
    return DialogType::Unknown;
}

// ============================================================
// isFileDialog — 纯只读高鲁棒性文件对话框判定
// ============================================================
bool DialogNavigator::isFileDialog(HWND hwnd, bool allowCurrentProcess) {
    if (!hwnd || !IsWindow(hwnd)) return false;

    wchar_t className[64] = {0};
    GetClassNameW(hwnd, className, 64);
    if (wcscmp(className, L"#32770") != 0) return false;

    DWORD pid = 0;
    GetWindowThreadProcessId(hwnd, &pid);
    if (pid == 0 || (!allowCurrentProcess && pid == GetCurrentProcessId())) return false;

    // 1. 绝对排除文件传输/复制/移动/删除等进度弹窗
    if (isProgressOrTransferDialog(hwnd)) {
        return false;
    }

    // 2. 标准属性表/选项卡对话框（含 ID_APPLY_NOW 0x3021/12321 应用按钮）绝对排除
    if (GetDlgItem(hwnd, 0x3021) != nullptr || GetDlgItem(hwnd, 12321) != nullptr) {
        return false;
    }

    // 3. 标题排查：Windows 资源管理器属性对话框多语言标题后缀排除
    wchar_t title[256] = {0};
    GetWindowTextW(hwnd, title, 256);
    std::wstring_view tv(title);
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
        if (tv.ends_with(suffix)) {
            return false;
        }
    }

    EnumChildContext ctx;
    EnumChildWindows(hwnd, EnumFileDialogChildren, reinterpret_cast<LPARAM>(&ctx));

    // 4. 包含进度条、动画或进度文本的窗口绝对排除
    if (ctx.hasProgressBar || ctx.hasAnimation || ctx.hasProgressText) return false;

    // 5. 包含多标签页（SysTabControl32）或应用按钮的属性表对话框绝对排除
    if (ctx.hasTabControl || ctx.hasApplyButton) return false;

    // 6. 必须具备确认按钮 (IDOK)。所有合法文件对话框与目录选择器均有确认选择机制；仅有取消按钮的进度/等待窗口绝对排除
    if (!ctx.okButtonHwnd) return false;

    // 包含向导后退按钮的复合向导页一律排除
    if (ctx.backButtonHwnd) return false;

    // 7. 真实文件/文件夹选择对话框核心特征正向识别：
    // (a) 现代文件对话框：必须拥有 Shell 文件列表视图 (SHELLDLL_DefView)
    if (ctx.hasDefView) {
        return true;
    }

    // (b) 经典文件对话框 (Legacy OPENFILENAME)：必须拥有专用文件名编辑框 (edt1: 1152 或 cmb13: 0x047C) 并配合下拉框或确认按钮
    if (ctx.standardFileEditHwnd && (ctx.comboBoxHwnd || ctx.okButtonHwnd)) {
        return true;
    }

    // (c) 目录/文件夹选择对话框：拥有命名空间导航树控件或标准树形控件 (NamespaceTreeControl / SysTreeView32) 且同时具备确认与取消按钮，无后退按钮，且控件总数不超过 18（排除包含树形控件的复杂向导组件页）
    if ((ctx.namespaceTreeHwnd || ctx.treeViewHwnd) && ctx.okButtonHwnd && ctx.cancelButtonHwnd && !ctx.backButtonHwnd && ctx.totalControls <= 18) {
        return true;
    }

    return false;
}

// ============================================================
// 子控件查找辅助（Legacy 模式专用）
// ============================================================
HWND DialogNavigator::findPathEditControl(HWND dialogHwnd) {
    if (!dialogHwnd || !IsWindow(dialogHwnd)) return nullptr;

    HWND bestEdit = nullptr;
    int maxTop = -1;
    struct Ctx { HWND* pBest; int* pMaxTop; } sCtx = { &bestEdit, &maxTop };

    EnumChildWindows(dialogHwnd, [](HWND hwnd, LPARAM lParam) -> BOOL {
        auto* ctx = reinterpret_cast<Ctx*>(lParam);
        wchar_t cls[64] = {0};
        GetClassNameW(hwnd, cls, 64);
        if (wcscmp(cls, L"Edit") == 0) {
            RECT rc = {}; GetWindowRect(hwnd, &rc);
            if (rc.top > *(ctx->pMaxTop)) { *(ctx->pMaxTop) = rc.top; *(ctx->pBest) = hwnd; }
        } else if (wcscmp(cls, L"ComboBoxEx32") == 0 || wcscmp(cls, L"ComboBox") == 0) {
            HWND ce = FindWindowExW(hwnd, nullptr, L"Edit", nullptr);
            if (ce) {
                RECT rc = {}; GetWindowRect(ce, &rc);
                if (rc.top > *(ctx->pMaxTop)) { *(ctx->pMaxTop) = rc.top; *(ctx->pBest) = ce; }
            }
        }
        return TRUE;
    }, reinterpret_cast<LPARAM>(&sCtx));

    return bestEdit;
}

HWND DialogNavigator::findAddressBandControl(HWND dialogHwnd) {
    if (!dialogHwnd || !IsWindow(dialogHwnd)) return nullptr;
    EnumChildContext ctx;
    EnumChildWindows(dialogHwnd, EnumFileDialogChildren, reinterpret_cast<LPARAM>(&ctx));
    return ctx.addressBandHwnd;
}

HWND DialogNavigator::findShellViewControl(HWND dialogHwnd) {
    if (!dialogHwnd || !IsWindow(dialogHwnd)) return nullptr;
    EnumChildContext ctx;
    EnumChildWindows(dialogHwnd, EnumFileDialogChildren, reinterpret_cast<LPARAM>(&ctx));
    return ctx.shellViewHwnd;
}

HWND DialogNavigator::findOkButtonControl(HWND dialogHwnd) {
    if (!dialogHwnd || !IsWindow(dialogHwnd)) return nullptr;
    EnumChildContext ctx;
    EnumChildWindows(dialogHwnd, EnumFileDialogChildren, reinterpret_cast<LPARAM>(&ctx));
    return ctx.okButtonHwnd;
}

// ============================================================
// UIAutomation 私有实现
// ============================================================

// Modern IFileDialog: 优先读取当前可见的 Address Band 面包屑；仅在用户
// 正在编辑地址时读取可见 Edit。只识别盘符/UNC 语法并验证目录存在，
// 不依赖“地址:”等任何本地化文本，也不接受隐藏控件里的历史值。
std::string DialogNavigator::uiaGetAddressBarPath(HWND dialogHwnd) {
    // Breadcrumb mode is authoritative. Windows can retain the address Edit
    // used by a previous programmatic navigation after the user has moved to
    // another folder; reading that hidden Edit first returns a stale path.
    struct NativeToolbarPath { std::wstring path; } toolbarPath;
    EnumChildWindows(dialogHwnd, [](HWND child, LPARAM parameter) -> BOOL {
        wchar_t className[32]{};
        GetClassNameW(child, className, static_cast<int>(std::size(className)));
        if (wcscmp(className, L"ToolbarWindow32") != 0 ||
            !isVisibleAddressControl(child)) return TRUE;
        auto* result = reinterpret_cast<NativeToolbarPath*>(parameter);
        result->path = extractExistingDirectory(getWindowTextWithTimeout(child));
        return result->path.empty() ? TRUE : FALSE;
    }, reinterpret_cast<LPARAM>(&toolbarPath));
    if (!toolbarPath.path.empty()) {
        return tools3000::core::WinUtils::wstringToUtf8(toolbarPath.path);
    }

    IUIAutomation* uia = getUIA();
    if (!uia) return "";

    ComPtr<IUIAutomationElement> dlgElem;
    if (FAILED(uia->ElementFromHandle(dialogHwnd, dlgElem.GetAddressOf())) || !dlgElem) return "";

    // Windows 11 exposes the breadcrumb address bar differently depending on
    // its current mode.  While the user is typing it is an Edit with id 41477;
    // after Enter it becomes an Address Band Root Pane (also id 41477) whose
    // child Pane (normally id 1001) owns a name such as "Address: D:\\Work".
    // Restrict the scan to that exact address-band subtree so localized labels,
    // search text and file-list cells can never be mistaken for a folder path.
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

    // The native Edit is valid only while it is genuinely visible (for
    // example while the user is typing a path). A hidden zero-sized Edit may
    // retain the last path Tools3000 submitted and must never win over the
    // live breadcrumb above.
    struct NativeAddress { HWND hwnd; } native{};
    EnumChildWindows(dialogHwnd, [](HWND child, LPARAM parameter) -> BOOL {
        auto* result = reinterpret_cast<NativeAddress*>(parameter);
        if (GetDlgCtrlID(child) != 41477 || !isVisibleAddressControl(child)) return TRUE;
        wchar_t className[32]{};
        GetClassNameW(child, className, static_cast<int>(std::size(className)));
        if (wcscmp(className, L"Edit") == 0) {
            result->hwnd = child;
            return FALSE;
        }
        return TRUE;
    }, reinterpret_cast<LPARAM>(&native));
    if (native.hwnd) {
        const std::wstring path = extractExistingDirectory(getWindowTextWithTimeout(native.hwnd));
        if (!path.empty()) return tools3000::core::WinUtils::wstringToUtf8(path);
    }
    VARIANT toolbarType{};
    toolbarType.vt = VT_I4;
    toolbarType.lVal = UIA_ToolBarControlTypeId;
    ComPtr<IUIAutomationCondition> toolbarCondition;
    ComPtr<IUIAutomationElementArray> toolbars;
    if (FAILED(uia->CreatePropertyCondition(UIA_ControlTypePropertyId, toolbarType,
                                            toolbarCondition.GetAddressOf())) || !toolbarCondition ||
        FAILED(dlgElem->FindAll(TreeScope_Descendants, toolbarCondition.Get(),
                                toolbars.GetAddressOf())) || !toolbars) return "";

    int count = 0;
    toolbars->get_Length(&count);
    for (int index = 0; index < count; ++index) {
        ComPtr<IUIAutomationElement> toolbar;
        if (FAILED(toolbars->GetElement(index, toolbar.GetAddressOf())) || !toolbar) continue;
        BSTR name = nullptr;
        if (FAILED(toolbar->get_CurrentName(&name)) || !name) continue;
        const std::wstring path = extractExistingDirectory(name);
        SysFreeString(name);
        if (!path.empty()) return tools3000::core::WinUtils::wstringToUtf8(path);
    }
    return "";
}

// Modern IFileDialog: 读取底部文件名输入框。优先使用稳定控件 ID 1152，
// 避免枚举文件列表中数十个虚拟 Edit 单元格。
std::string DialogNavigator::uiaGetFileNameText(HWND dialogHwnd, bool* controlFound) {
    if (controlFound) *controlFound = false;
    struct NativeFileName { HWND hwnd; } native{};
    EnumChildWindows(dialogHwnd, [](HWND child, LPARAM parameter) -> BOOL {
        auto* result = reinterpret_cast<NativeFileName*>(parameter);
        if (GetDlgCtrlID(child) != 1152) return TRUE;
        wchar_t className[32]{};
        GetClassNameW(child, className, static_cast<int>(std::size(className)));
        if (wcscmp(className, L"Edit") == 0) {
            result->hwnd = child;
            return FALSE;
        }
        return TRUE;
    }, reinterpret_cast<LPARAM>(&native));
    if (native.hwnd) {
        if (controlFound) *controlFound = true;
        const std::wstring text = getWindowTextWithTimeout(native.hwnd);
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

namespace {

// FOS_PICKFOLDERS dialogs normally have no file-name Edit. In that mode the
// authoritative user intent is the selected child in the Shell Items View,
// while the address bar still represents only its parent folder.
std::string uiaGetSelectedShellChild(HWND dialogHwnd, const std::string& currentFolder) {
    if (currentFolder.empty()) return {};

    IUIAutomation* uia = getUIA();
    if (!uia) return {};

    ComPtr<IUIAutomationElement> dialogElement;
    if (FAILED(uia->ElementFromHandle(dialogHwnd, dialogElement.GetAddressOf())) ||
        !dialogElement) {
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

    const std::filesystem::path parent(
        tools3000::core::WinUtils::utf8ToWstring(currentFolder));
    const std::filesystem::path candidate = (parent / itemName).lexically_normal();
    const DWORD attributes = GetFileAttributesW(candidate.c_str());
    if (attributes == INVALID_FILE_ATTRIBUTES) return {};
    return tools3000::core::WinUtils::wstringToUtf8(candidate.native());
}

} // namespace

// Modern IFileDialog: UIA 地址栏导航核心。
// Windows Shell 为地址编辑框暴露稳定、非本地化的 AutomationId 41477；
// 底部文件名框是 1152，搜索框是 SearchEditBox。只有同时通过 ID、类型、
// 窗口归属和几何校验的控件才允许写入。
bool DialogNavigator::uiaNavigate(HWND dialogHwnd, const std::wstring& wPath) {
    RECT dialogRect{};
    if (!GetWindowRect(dialogHwnd, &dialogRect)) return false;
    const LONG dialogWidth = dialogRect.right - dialogRect.left;
    const LONG dialogHeight = dialogRect.bottom - dialogRect.top;
    if (dialogWidth <= 0 || dialogHeight <= 0) return false;

    auto isSafeAddressHwnd = [&](HWND editHwnd) -> bool {
        if (!editHwnd || !IsWindow(editHwnd) || GetDlgCtrlID(editHwnd) != 41477) return false;
        wchar_t className[32]{};
        GetClassNameW(editHwnd, className, static_cast<int>(std::size(className)));
        if (wcscmp(className, L"Edit") != 0) return false;
        RECT editRect{};
        if (!GetWindowRect(editHwnd, &editRect)) return false;
        const LONG width = editRect.right - editRect.left;
        const LONG height = editRect.bottom - editRect.top;
        const LONG centerX = editRect.left + width / 2;
        return (GetAncestor(editHwnd, GA_ROOT) == dialogHwnd || IsChild(dialogHwnd, editHwnd)) &&
               editRect.top >= dialogRect.top &&
               editRect.bottom <= dialogRect.top + dialogHeight * 45 / 100 &&
               centerX <= dialogRect.left + dialogWidth * 70 / 100 &&
               width >= 120 && height >= 12;
    };

    auto submitNativeAddress = [&](HWND editHwnd, const char* source) -> bool {
        if (!isSafeAddressHwnd(editHwnd)) return false;
        LRESULT ignored = 0;
        if (!sendMessageWithTimeout(editHwnd, WM_SETTEXT, 0,
                                    reinterpret_cast<LPARAM>(wPath.c_str()), ignored, 250)) {
            return false;
        }
        const LPARAM keyDown = 1 | (static_cast<LPARAM>(MapVirtualKeyW(VK_RETURN, MAPVK_VK_TO_VSC)) << 16);
        const LPARAM keyUp = keyDown | (static_cast<LPARAM>(1) << 30) | (static_cast<LPARAM>(1) << 31);
        if (!sendMessageWithTimeout(editHwnd, WM_KEYDOWN, VK_RETURN, keyDown, ignored, 250) ||
            !sendMessageWithTimeout(editHwnd, WM_KEYUP, VK_RETURN, keyUp, ignored, 250)) {
            return false;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(180));
        if (!IsWindow(dialogHwnd)) {
            LOG_ERROR("UIA: 地址栏回车后对话框意外关闭");
            return false;
        }
        LOG_INFO("UIA: {} 已向地址栏安全提交导航: {}", source,
                 tools3000::core::WinUtils::wstringToUtf8(wPath));
        return true;
    };

    // 首选原生 HWND：不依赖前台焦点，也不会触发 UIA 大树遍历。41477 是
    // Shell 地址 Edit 的控件 ID；底部输入框 1152 在入口处即被排除。
    struct AddressSearch { HWND result; } addressSearch{};
    for (int retry = 0; retry < 20 && !addressSearch.result && IsWindow(dialogHwnd); ++retry) {
        EnumChildWindows(dialogHwnd, [](HWND child, LPARAM parameter) -> BOOL {
            auto* search = reinterpret_cast<AddressSearch*>(parameter);
            if (GetDlgCtrlID(child) != 41477) return TRUE;
            wchar_t className[32]{};
            GetClassNameW(child, className, static_cast<int>(std::size(className)));
            if (wcscmp(className, L"Edit") == 0) {
                search->result = child;
                return FALSE;
            }
            return TRUE;
        }, reinterpret_cast<LPARAM>(&addressSearch));
        if (!addressSearch.result) std::this_thread::sleep_for(std::chrono::milliseconds(25));
    }
    if (addressSearch.result && submitNativeAddress(addressSearch.result, "native id=41477")) {
        return true;
    }

    IUIAutomation* uia = getUIA();
    if (!uia) {
        LOG_WARN("UIA: IUIAutomation 接口不可用");
        return false;
    }
    ComPtr<IUIAutomationElement> dialogElement;
    if (FAILED(uia->ElementFromHandle(dialogHwnd, dialogElement.GetAddressOf())) || !dialogElement) {
        return false;
    }

    auto commitAddress = [&](IUIAutomationElement* element, const char* source) -> bool {
        if (!element || !IsWindow(dialogHwnd)) return false;

        CONTROLTYPEID controlType = 0;
        UIA_HWND nativeHandle{};
        RECT editRect{};
        BSTR automationId = nullptr;
        if (FAILED(element->get_CurrentControlType(&controlType)) ||
            FAILED(element->get_CurrentNativeWindowHandle(&nativeHandle)) || !nativeHandle ||
            FAILED(element->get_CurrentBoundingRectangle(&editRect)) ||
            FAILED(element->get_CurrentAutomationId(&automationId))) {
            SysFreeString(automationId);
            return false;
        }

        const bool exactAddressId = automationId && wcscmp(automationId, L"41477") == 0;
        SysFreeString(automationId);
        const HWND editHwnd = reinterpret_cast<HWND>(nativeHandle);
        const LONG editWidth = editRect.right - editRect.left;
        const LONG editHeight = editRect.bottom - editRect.top;
        const LONG editCenterX = editRect.left + editWidth / 2;
        const bool belongsToDialog = GetAncestor(editHwnd, GA_ROOT) == dialogHwnd ||
                                     IsChild(dialogHwnd, editHwnd);
        const bool inUpperBand = editRect.top >= dialogRect.top &&
                                 editRect.bottom <= dialogRect.top + dialogHeight * 45 / 100;
        const bool notSearchBox = editCenterX <= dialogRect.left + dialogWidth * 70 / 100;
        const bool usableSize = editWidth >= 120 && editHeight >= 12;
        if (controlType != UIA_EditControlTypeId || !exactAddressId || !belongsToDialog ||
            !inUpperBand || !notSearchBox || !usableSize) {
            LOG_WARN("UIA: {} 候选未通过地址栏硬校验 id={}, belongs={}, upper={}, "
                     "nonSearch={}, size={}", source, exactAddressId, belongsToDialog,
                     inUpperBand, notSearchBox, usableSize);
            return false;
        }

        ComPtr<IUIAutomationValuePattern> valuePattern;
        if (FAILED(element->GetCurrentPatternAs(
                UIA_ValuePatternId, IID_IUIAutomationValuePattern,
                reinterpret_cast<void**>(valuePattern.GetAddressOf()))) || !valuePattern) {
            return false;
        }
        BSTR pathValue = SysAllocString(wPath.c_str());
        const HRESULT setResult = valuePattern->SetValue(pathValue);
        SysFreeString(pathValue);
        if (FAILED(setResult)) {
            LOG_WARN("UIA: 地址栏 ValuePattern::SetValue 失败 hr=0x{:X}",
                     static_cast<unsigned>(setResult));
            return false;
        }

        // 回车直接投递给已严格识别的地址 Edit。相比全局 SendInput，
        // 不依赖前台抢焦点，也不可能触发底部默认确认按钮。
        element->SetFocus();
        return submitNativeAddress(editHwnd, source);
    };

    VARIANT addressId{};
    addressId.vt = VT_BSTR;
    addressId.bstrVal = SysAllocString(L"41477");
    ComPtr<IUIAutomationCondition> addressCondition;
    const HRESULT conditionResult = uia->CreatePropertyCondition(
        UIA_AutomationIdPropertyId, addressId, addressCondition.GetAddressOf());
    VariantClear(&addressId);
    VARIANT editType{};
    editType.vt = VT_I4;
    editType.lVal = UIA_EditControlTypeId;
    ComPtr<IUIAutomationCondition> editCondition;
    ComPtr<IUIAutomationCondition> exactAddressCondition;
    if (SUCCEEDED(conditionResult) && addressCondition &&
        SUCCEEDED(uia->CreatePropertyCondition(UIA_ControlTypePropertyId, editType,
                                               editCondition.GetAddressOf())) && editCondition) {
        uia->CreateAndCondition(addressCondition.Get(), editCondition.Get(),
                                exactAddressCondition.GetAddressOf());
    }
    if (exactAddressCondition) {
        ComPtr<IUIAutomationElement> addressElement;
        if (SUCCEEDED(dialogElement->FindFirst(TreeScope_Descendants, exactAddressCondition.Get(),
                                               addressElement.GetAddressOf())) && addressElement &&
            commitAddress(addressElement.Get(), "AutomationId=41477")) {
            return true;
        }
    }

    const std::string processName = tools3000::core::WinUtils::getProcessNameFromWindow(dialogHwnd);
    if (!isInstallerProcess(processName) && !isInstallerOrWizard(dialogHwnd)) {
        struct Activator { WORD modifier; WORD key; const char* name; };
        constexpr Activator activators[] = {
            {VK_MENU, 'D', "Alt+D"},
            {VK_CONTROL, 'L', "Ctrl+L"},
            {0, VK_F4, "F4"},
        };

        for (const auto& activator : activators) {
            if (!sendKeyChord(dialogHwnd, activator.modifier, activator.key)) {
                LOG_WARN("DialogNavigator: UIA {} could not be sent to target dialog", activator.name);
                continue;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(90));
            if (!IsWindow(dialogHwnd) || !isTargetForeground(dialogHwnd)) return false;

            ComPtr<IUIAutomationElement> focusedElem;
            HRESULT hr = uia->GetFocusedElement(focusedElem.GetAddressOf());
            if (FAILED(hr) || !focusedElem) continue;

            if (commitAddress(focusedElem.Get(), activator.name)) return true;
        }
    }

    LOG_WARN("DialogNavigator: address bar navigation safety checks not passed, skipping");
    return false;
}


// ============================================================
// getCurrentDialogFolder — Modern + FolderPicker + Legacy 安全多模
// ============================================================
std::string DialogNavigator::getCurrentDialogFolder(HWND dialogHwnd) {
    if (!dialogHwnd || !IsWindow(dialogHwnd)) return "";

    DialogType dtype = detectDialogType(dialogHwnd);

    if (dtype == DialogType::Modern) {
        // Modern: UIA 读取地址栏路径
        std::string uiaPath = uiaGetAddressBarPath(dialogHwnd);
        if (!uiaPath.empty()) {
            return uiaPath;
        }
    }

    if (dtype == DialogType::FolderPicker) {
        // FolderPicker (如 SHBrowseForFolder 带 BIF_EDITBOX): 安全读取 Edit 内容
        HWND edit = findPathEditControl(dialogHwnd);
        if (edit) {
            std::wstring text = getWindowTextWithTimeout(edit, MAX_PATH);
            if (!text.empty()) {
                DWORD attr = GetFileAttributesW(text.c_str());
                if (attr != INVALID_FILE_ATTRIBUTES && (attr & FILE_ATTRIBUTE_DIRECTORY)) {
                    return tools3000::core::WinUtils::wstringToUtf8(text);
                }
            }
        }
    }

    // 严禁发送 CDM_GETFOLDERPATH (WM_USER+102)！因其跨进程传递裸指针且缺乏 OS 编组，会导致目标进程内存非法访问 (0xC0000005) 崩溃！
    // 降级方案：枚举子窗口文字寻找已存在的物理目录绝对路径（基于 User32 WM_GETTEXT 安全内核级跨进程编组）
    std::string foundPath;
    EnumChildWindows(dialogHwnd, [](HWND hwnd, LPARAM lParam) -> BOOL {
        auto* outStr = reinterpret_cast<std::string*>(lParam);
        wchar_t buf[MAX_PATH * 2] = {0};
        GetWindowTextW(hwnd, buf, MAX_PATH * 2);
        std::wstring text = buf;
        if (text.size() >= 3 && iswalpha(text[0]) && text[1] == L':' && text[2] == L'\\') {
            DWORD attr = GetFileAttributesW(text.c_str());
            if (attr != INVALID_FILE_ATTRIBUTES && (attr & FILE_ATTRIBUTE_DIRECTORY)) {
                *outStr = tools3000::core::WinUtils::wstringToUtf8(text);
                return FALSE;
            }
        }
        return TRUE;
    }, reinterpret_cast<LPARAM>(&foundPath));
    return foundPath;
}

// ============================================================
// getSelectedPath — Modern + FolderPicker + Legacy 安全多模
// ============================================================
std::string DialogNavigator::getSelectedPath(HWND dialogHwnd) {
    if (!dialogHwnd || !IsWindow(dialogHwnd)) return "";

    DialogType dtype = detectDialogType(dialogHwnd);

    if (dtype == DialogType::Modern) {
        // Modern: UIA 读取底部文件名框
        bool fileNameControlFound = false;
        std::string fileName = uiaGetFileNameText(dialogHwnd, &fileNameControlFound);
        if (!fileName.empty()) {
            std::wstring wFileName = tools3000::core::WinUtils::utf8ToWstring(fileName);
            if (GetFileAttributesW(wFileName.c_str()) != INVALID_FILE_ATTRIBUTES) {
                return fileName;
            }
            std::string curFolder = getCurrentDialogFolder(dialogHwnd);
            if (!curFolder.empty()) {
                const std::filesystem::path cand =
                    std::filesystem::path(tools3000::core::WinUtils::utf8ToWstring(curFolder)) /
                    tools3000::core::WinUtils::utf8ToWstring(fileName);
                std::error_code ec;
                if (std::filesystem::exists(cand, ec)) {
                    return tools3000::core::WinUtils::wstringToUtf8(cand.native());
                }
            }
        }
        // 选择文件夹对话框通常没有文件名输入框。此时必须读取 Items View
        // 中真正选中的子目录，不能把地址栏的当前父目录误当成用户选择。
        if (!fileNameControlFound) {
            const std::string currentFolder = getCurrentDialogFolder(dialogHwnd);
            const std::string selectedChild =
                uiaGetSelectedShellChild(dialogHwnd, currentFolder);
            if (!selectedChild.empty()) return selectedChild;
        }
        return "";
    }

    // 严禁发送 CDM_GETFILEPATH (WM_USER+101)！
    // 安全方式：读取输入控件文本
    HWND editHwnd = findPathEditControl(dialogHwnd);
    if (editHwnd && IsWindow(editHwnd)) {
        std::wstring editStr = getWindowTextWithTimeout(editHwnd, MAX_PATH);
        if (!editStr.empty()) {
            if (GetFileAttributesW(editStr.c_str()) != INVALID_FILE_ATTRIBUTES) {
                return tools3000::core::WinUtils::wstringToUtf8(editStr);
            }
            std::string curFolder = getCurrentDialogFolder(dialogHwnd);
            if (!curFolder.empty()) {
                const std::filesystem::path cand =
                    std::filesystem::path(tools3000::core::WinUtils::utf8ToWstring(curFolder)) /
                    editStr;
                std::error_code ec;
                if (std::filesystem::exists(cand, ec)) {
                    return tools3000::core::WinUtils::wstringToUtf8(cand.native());
                }
            }
        }
    }
    return "";
}

// ============================================================
// navigateToFolder — Modern UIAutomation + Legacy 安全多模
// ============================================================
bool DialogNavigator::navigateToFolder(HWND dialogHwnd, const std::string& folderPath) {
    if (!dialogHwnd || !IsWindow(dialogHwnd) || folderPath.empty()) return false;

    std::wstring wPath = tools3000::core::WinUtils::utf8ToWstring(folderPath);
    if (wPath.empty()) return false;
    DWORD attrs = GetFileAttributesW(wPath.c_str());
    if (attrs == INVALID_FILE_ATTRIBUTES || (attrs & FILE_ATTRIBUTE_DIRECTORY) == 0) {
        LOG_WARN("DialogNavigator: reject navigating to non-directory or non-existent path: {}", folderPath);
        return false;
    }

    DialogType dtype = detectDialogType(dialogHwnd);
    LOG_INFO("DialogNavigator: navigating dialog hwnd=0x{:X}, type={}, path={}",
             reinterpret_cast<uintptr_t>(dialogHwnd),
             static_cast<int>(dtype), folderPath);

    if (dtype == DialogType::Modern) {
        // Modern: UIAutomation 地址栏导航（唯一可靠方案）
        bool ok = uiaNavigate(dialogHwnd, wPath);
        if (!ok) {
            LOG_WARN("DialogNavigator: UIA modern navigation failed for {}", folderPath);
        }
        return ok;
    }

    // 检查宿主窗口及进程是否属于安装向导程序
    std::string processName = tools3000::core::WinUtils::getProcessNameFromWindow(dialogHwnd);
    const bool isInstaller = isInstallerOrWizard(dialogHwnd) || isInstallerProcess(processName);

    // FolderPicker: 安全设置 Edit 控件内容，绝不发送 VK_RETURN，绝不恢复 oldText
    if (dtype == DialogType::FolderPicker) {
        HWND editHwnd = findPathEditControl(dialogHwnd);
        if (!editHwnd) {
            LOG_WARN("DialogNavigator: FolderPicker has no edit control, skipping: hwnd=0x{:X}", reinterpret_cast<uintptr_t>(dialogHwnd));
            return false;
        }
        DWORD_PTR ignored = 0;
        if (!SendMessageTimeoutW(editHwnd, WM_SETTEXT, 0,
                                 reinterpret_cast<LPARAM>(wPath.c_str()),
                                 SMTO_ABORTIFHUNG | SMTO_BLOCK, 250, &ignored)) {
            return false;
        }
        HWND parent = GetParent(editHwnd);
        if (parent) {
            SendMessageTimeoutW(parent, WM_COMMAND,
                                MAKEWPARAM(GetDlgCtrlID(editHwnd), EN_CHANGE),
                                reinterpret_cast<LPARAM>(editHwnd),
                                SMTO_ABORTIFHUNG | SMTO_BLOCK, 250, &ignored);
        }
        LOG_INFO("DialogNavigator: FolderPicker directory updated safely via WM_SETTEXT without VK_RETURN: {}", folderPath);
        return true;
    }

    HWND editHwnd = findPathEditControl(dialogHwnd);
    if (!editHwnd) {
        LOG_WARN("DialogNavigator: path edit control not found, hwnd=0x{:X}", reinterpret_cast<uintptr_t>(dialogHwnd));
        return false;
    }

    // 安装程序向导严禁发送 VK_RETURN！单行文本框收到回车会被转为向导的默认确认按钮（Next/Install），导致向导提前安装或状态紊乱崩溃
    if (isInstaller) {
        DWORD_PTR ignored = 0;
        if (!SendMessageTimeoutW(editHwnd, WM_SETTEXT, 0,
                                 reinterpret_cast<LPARAM>(wPath.c_str()),
                                 SMTO_ABORTIFHUNG | SMTO_BLOCK, 250, &ignored)) {
            return false;
        }
        HWND parent = GetParent(editHwnd);
        if (parent) {
            SendMessageTimeoutW(parent, WM_COMMAND,
                                MAKEWPARAM(GetDlgCtrlID(editHwnd), EN_CHANGE),
                                reinterpret_cast<LPARAM>(editHwnd),
                                SMTO_ABORTIFHUNG | SMTO_BLOCK, 250, &ignored);
        }
        LOG_INFO("DialogNavigator: installer directory updated safely via WM_SETTEXT without VK_RETURN: {}", folderPath);
        return true;
    }

    // Legacy OPENFILENAME 标准对话框导航流程
    std::wstring wPathDir = wPath;
    if (wPathDir.back() != L'\\' && wPathDir.back() != L'/') wPathDir.push_back(L'\\');

    const int oldLength = GetWindowTextLengthW(editHwnd);
    std::wstring oldText(static_cast<size_t>(std::max(0, oldLength)), L'\0');
    if (oldLength > 0) {
        GetWindowTextW(editHwnd, oldText.data(), oldLength + 1);
    }

    DWORD_PTR ignored = 0;
    if (!SendMessageTimeoutW(editHwnd, WM_SETTEXT, 0,
                             reinterpret_cast<LPARAM>(wPathDir.c_str()),
                             SMTO_ABORTIFHUNG | SMTO_BLOCK, 250, &ignored)) {
        return false;
    }
    HWND parent = GetParent(editHwnd);
    if (parent) {
        SendMessageTimeoutW(parent, WM_COMMAND,
                            MAKEWPARAM(GetDlgCtrlID(editHwnd), EN_CHANGE),
                            reinterpret_cast<LPARAM>(editHwnd),
                            SMTO_ABORTIFHUNG | SMTO_BLOCK, 250, &ignored);
    }

    SendMessageTimeoutW(editHwnd, WM_KEYDOWN, VK_RETURN, 0x001C0001,
                        SMTO_ABORTIFHUNG | SMTO_BLOCK, 250, &ignored);
    SendMessageTimeoutW(editHwnd, WM_KEYUP, VK_RETURN, 0xC01C0001,
                        SMTO_ABORTIFHUNG | SMTO_BLOCK, 250, &ignored);

    std::this_thread::sleep_for(std::chrono::milliseconds(80));
    if (!IsWindow(dialogHwnd)) {
        LOG_ERROR("DialogNavigator: dialog closed unexpectedly after navigation: {}", folderPath);
        return false;
    }

    SendMessageTimeoutW(editHwnd, WM_SETTEXT, 0,
                        reinterpret_cast<LPARAM>(oldText.c_str()),
                        SMTO_ABORTIFHUNG | SMTO_BLOCK, 250, &ignored);
    LOG_INFO("DialogNavigator: legacy navigation completed: {}", folderPath);
    return true;
}

} // namespace tools3000::dialog

