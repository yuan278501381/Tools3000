/**
 * Tools3000 - High Performance Windows Productivity Suite
 *
 * Copyright (c) 2026 Yy1 (yuan278501381) & Tools3000 contributors
 *
 * Licensed under the GPL-3.0 License.
 */

#pragma once

#include <windows.h>
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cwctype>
#include <string>
#include <string_view>

namespace tools3000::dialog {

/// 文件对话框架构 5 类 MECE 分类
enum class DialogArchitecture : uint8_t {
    Unknown = 0,
    ModernIFileDialog = 1,   // Vista+ COM IFileOpenDialog / IFileSaveDialog (DUIViewWndClassName / DirectUIHWND / Address Band Root)
    LegacyExplorer = 2,      // OFN_EXPLORER: 含 SHELLDLL_DefView 但无 DirectUIHWND / Address Band
    ClassicOpenFileName = 3, // 经典 OPENFILENAME: 含 edt1 (1152) / cmb13 (1148) 但无 SHELLDLL_DefView
    BrowseForFolder = 4,     // SHBrowseForFolder (SysTreeView32 / NamespaceTreeControl)
};

/// 文件对话框业务模式 3 类分类（无法明确时保持 Unknown，按最安全模式处理）
enum class DialogMode : uint8_t {
    Unknown = 0,
    OpenFile = 1,
    SaveFile = 2,
    PickFolder = 3,
};

/// 对话框子控件结构信号（由单次 EnumChildWindows 采集，供纯函数分类与单元测试复用）
struct DialogStructureSignals {
    bool hasDefView{false};
    bool hasDirectUI{false};           // DUIViewWndClassName / DirectUIHWND
    bool hasAddressBand{false};        // Address Band Root / Breadcrumb Parent
    bool hasFileTypeCombo1136{false};  // ComboBox ID 1136 (cmb1 文件类型下拉框)
    bool hasFileNameEdit1152{false};   // Edit ID 1152 (edt1)
    bool hasFileNameCombo1148{false};  // ComboBoxEx32 / ComboBox ID 1148 (cmb13)
    bool hasAnyComboBox{false};
    bool hasTreeView{false};           // SysTreeView32 / NamespaceTreeControl
    bool hasOkButton{false};
    bool hasCancelButton{false};
    bool hasBackButton{false};
    bool hasApplyButton{false};
    bool hasTabControl{false};
    bool hasProgressIndicator{false};
    int totalControls{0};
};

/// 对话框模式判定信号
struct DialogModeSignals {
    DialogArchitecture architecture{DialogArchitecture::Unknown};
    bool hasDirectUI{false};
    bool hasDefView{false};
    bool hasFileTypeCombo1136{false};
    bool hasFileNameEdit1152{false};
    bool fileNameEditHasInitialText{false};
    std::wstring okButtonText;
    std::wstring dialogTitle;
    std::wstring systemOpenButtonText;  // 来自 comdlg32.dll MUI 的系统"打开"按钮文本
    std::wstring systemSaveButtonText;  // 来自 comdlg32.dll MUI 的系统"保存"按钮文本
};

/// 内嵌搜索选中结果的安全动作决策
enum class SearchActionKind : uint8_t {
    NavigateFolderOnly = 0,          // 仅跳转目录（绝不修改底部文件名、绝不提交）
    NavigateAndFillFileName = 1,     // 跳转父目录并填入文件名（不自动提交）
    NavigateFillAndConfirmOpen = 2,  // 跳转父目录、填入文件名并触发打开（仅 OpenFile + Shift+Enter）
};

struct SearchActionDecision {
    SearchActionKind kind{SearchActionKind::NavigateFolderOnly};
    bool useParentDirectory{false};
};

inline bool shouldProcessDialogWinEvent(
    DWORD event,
    HWND hwnd,
    LONG idObject,
    LONG idChild
) noexcept {
    if (!hwnd) return false;
    // INVOKED 事件可能来自确认/取消按钮子窗口或无障碍子项，必须保留；
    // 其余事件只接收窗口自身，避免地址栏 Edit 隐藏时被误判为整个对话框关闭。
    if (event != EVENT_OBJECT_INVOKED && (idObject > 0 || idChild != CHILDID_SELF)) {
        return false;
    }
    return true;
}

/// 无 C-locale 依赖的 Unicode 小写折叠（覆盖 ASCII、拉丁扩展与西里尔字母，杜绝 "C" locale 下俄语大写无法匹配）
inline wchar_t foldUnicodeLowerInvariant(wchar_t ch) noexcept {
    if (ch >= L'A' && ch <= L'Z') {
        return static_cast<wchar_t>(ch + (L'a' - L'A'));
    }
    // Latin-1 Supplement 大写 (À..Ö, Ø..Þ)
    if ((ch >= 0x00C0 && ch <= 0x00D6) || (ch >= 0x00D8 && ch <= 0x00DE)) {
        return static_cast<wchar_t>(ch + 0x20);
    }
    // Cyrillic 基本大写 (А..Я -> а..я) 与 Ё -> ё
    if (ch >= 0x0410 && ch <= 0x042F) {
        return static_cast<wchar_t>(ch + 0x20);
    }
    if (ch == 0x0401) {
        return static_cast<wchar_t>(0x0451);
    }
    return ch;
}

/// 标准化按钮/标题文本：移除 '&' 助记符、括号快捷键（如 (&O)/(&S)）与首尾空白，并转为小写
inline std::wstring normalizeDialogUiLabel(std::wstring_view raw) {
    std::wstring cleaned;
    cleaned.reserve(raw.size());
    for (size_t i = 0; i < raw.size(); ++i) {
        const wchar_t ch = raw[i];
        if (ch == L'&') continue;
        // 剥离尾部如 "(&O)" / "(O)" / "（O）" 类助记括号
        if ((ch == L'(' || ch == L'\xFF08') && i + 2 < raw.size()) {
            const size_t closeParen = raw.find_first_of(L")\xFF09", i + 1);
            if (closeParen != std::wstring_view::npos && closeParen - i <= 4) {
                i = closeParen;
                continue;
            }
        }
        cleaned.push_back(foldUnicodeLowerInvariant(ch));
    }
    while (!cleaned.empty() && (std::iswspace(cleaned.front()) || cleaned.front() == L'.' || cleaned.front() == L'\x2026')) {
        cleaned.erase(cleaned.begin());
    }
    while (!cleaned.empty() && (std::iswspace(cleaned.back()) || cleaned.back() == L'.' || cleaned.back() == L'\x2026')) {
        cleaned.pop_back();
    }
    return cleaned;
}

/// MECE 5 类文件对话框架构纯函数分类器
inline DialogArchitecture classifyDialogArchitecture(const DialogStructureSignals& s) noexcept {
    if (s.hasBackButton || s.hasApplyButton || s.hasTabControl || s.hasProgressIndicator) {
        return DialogArchitecture::Unknown;
    }
    if (!s.hasOkButton) {
        return DialogArchitecture::Unknown;
    }

    // 1. ModernIFileDialog: 具备 DUIViewWndClassName / DirectUIHWND 或 Address Band Root / Breadcrumb Parent
    if ((s.hasDirectUI || s.hasAddressBand) && (s.hasDefView || (s.hasDirectUI && s.hasAddressBand))) {
        return DialogArchitecture::ModernIFileDialog;
    }

    // 2. LegacyExplorer (OFN_EXPLORER): 含 SHELLDLL_DefView 但无 DirectUIHWND 且无 Address Band
    if (s.hasDefView) {
        return DialogArchitecture::LegacyExplorer;
    }

    // 3. BrowseForFolder: 含 SysTreeView32 / NamespaceTreeControl + 确认 + 取消，且控件数在合理范围内
    if (s.hasTreeView && s.hasCancelButton && s.totalControls <= 18 &&
        !s.hasFileNameEdit1152 && !s.hasFileNameCombo1148) {
        return DialogArchitecture::BrowseForFolder;
    }

    // 4. ClassicOpenFileName: 经典 OPENFILENAME（具备 edt1:1152 或 cmb13:1148）
    if (s.hasFileNameEdit1152 || s.hasFileNameCombo1148) {
        return DialogArchitecture::ClassicOpenFileName;
    }

    return DialogArchitecture::Unknown;
}

/// 文件对话框 3 模式精确分类器（结合结构指纹 + comdlg32.dll MUI + 8 语种词表 + 预填保守安全兜底）
inline DialogMode classifyDialogMode(const DialogModeSignals& s) {
    if (s.architecture == DialogArchitecture::Unknown) {
        return DialogMode::Unknown;
    }
    if (s.architecture == DialogArchitecture::BrowseForFolder) {
        return DialogMode::PickFolder;
    }

    const std::wstring okNorm = normalizeDialogUiLabel(s.okButtonText);
    const std::wstring titleNorm = normalizeDialogUiLabel(s.dialogTitle);
    const std::wstring sysOpenNorm = normalizeDialogUiLabel(s.systemOpenButtonText);
    const std::wstring sysSaveNorm = normalizeDialogUiLabel(s.systemSaveButtonText);

    auto containsAny = [](const std::wstring& hay, std::initializer_list<std::wstring_view> needles) -> bool {
        if (hay.empty()) return false;
        for (const auto& n : needles) {
            if (!n.empty() && hay.find(n) != std::wstring::npos) return true;
        }
        return false;
    };

    // 1. 优先检查全 8 语种明确的“选择文件夹”按钮/标题词表
    //    注意：Win11 现代 IFileOpenDialog / IFileSaveDialog 在未调用 SetFileTypes 时可能不含
    //    Win32 ComboBox 1136，因此绝不能在检查确定按钮文案前凭 !hasFileTypeCombo1136 提前误判为 PickFolder！
    static constexpr std::initializer_list<std::wstring_view> kPickFolderKeywords = {
        L"选择文件夹", L"选择目录", L"選擇資料夾", L"選擇目錄",
        L"select folder", L"choose folder", L"pick folder", L"browse for folder",
        L"フォルダーの選択", L"フォルダの選択", L"フォルダーを選択",
        L"폴더 선택",
        L"выбор папки", L"выбрать папку",
        L"selecionar pasta", L"escolher pasta",
        L"seleccionar carpeta", L"elegir carpeta"
    };
    if (containsAny(okNorm, kPickFolderKeywords)) {
        return DialogMode::PickFolder;
    }

    // 2. 按钮文本精确匹配 comdlg32.dll 系统 MUI 标准按钮文本
    if (!sysSaveNorm.empty() && okNorm == sysSaveNorm) {
        return DialogMode::SaveFile;
    }
    if (!sysOpenNorm.empty() && okNorm == sysOpenNorm) {
        // 二次防御：如果标题显式包含“另存为/Save As”，绝不误判为 OpenFile
        static constexpr std::initializer_list<std::wstring_view> kSaveTitleGuards = {
            L"另存为", L"另存新檔", L"另存新档", L"save as", L"名前を付けて保存",
            L"다른 이름으로 저장", L"сохранить как", L"salvar como", L"guardar como"
        };
        if (containsAny(titleNorm, kSaveTitleGuards)) {
            return DialogMode::SaveFile;
        }
        return DialogMode::OpenFile;
    }

    // 3. 全 8 语种 Save / Open 按钮兜底词表（优先检查 Save，宁可保守绝不误覆盖保存文件名）
    static constexpr std::initializer_list<std::wstring_view> kSaveKeywords = {
        L"保存", L"另存", L"导出", L"儲存", L"匯出",
        L"save", L"export",
        L"書き出し", L"エクスポート",
        L"저장", L"내보내기",
        L"сохранить", L"экспорт",
        L"salvar", L"exportar",
        L"guardar"
    };
    static constexpr std::initializer_list<std::wstring_view> kOpenKeywords = {
        L"打开", L"导入", L"開啟", L"匯入",
        L"open", L"import", L"choose file", L"select file",
        L"開く", L"インポート",
        L"열기", L"가져오기",
        L"открыть", L"импорт",
        L"abrir", L"importar"
    };

    if (containsAny(okNorm, kSaveKeywords)) {
        return DialogMode::SaveFile;
    }
    if (containsAny(titleNorm, kSaveKeywords)) {
        return DialogMode::SaveFile;
    }
    if (containsAny(okNorm, kOpenKeywords)) {
        return DialogMode::OpenFile;
    }
    if (containsAny(titleNorm, kOpenKeywords)) {
        // 如果文件名输入框初始已预填默认名称且按钮不是明确的“打开”（如“确定/OK”），按保守策略归类为 SaveFile/Unknown
        if (s.fileNameEditHasInitialText && okNorm != L"open" && okNorm != L"打开" && okNorm != L"開啟") {
            return DialogMode::SaveFile;
        }
        return DialogMode::OpenFile;
    }

    // 4. 按钮与标题均未命中明确词表时，再结合 Modern PickFolder 结构指纹与预填文件名兜底
    if (s.architecture == DialogArchitecture::ModernIFileDialog &&
        s.hasDirectUI && s.hasDefView && !s.hasFileTypeCombo1136 &&
        !s.fileNameEditHasInitialText) {
        return DialogMode::PickFolder;
    }
    if (!s.hasFileTypeCombo1136 && !s.hasFileNameEdit1152 && containsAny(titleNorm, kPickFolderKeywords)) {
        return DialogMode::PickFolder;
    }
    if (s.fileNameEditHasInitialText) {
        return DialogMode::SaveFile;
    }
    return DialogMode::Unknown;
}

/// 内嵌搜索确认跳转安全策略（Save As / Unknown 铁壁保护）
inline SearchActionDecision decideInlineSearchAction(
    DialogMode mode,
    bool itemIsDirectory,
    bool ctrlPressed,
    bool shiftPressed
) noexcept {
    if (itemIsDirectory) {
        return {SearchActionKind::NavigateFolderOnly, false};
    }
    if (ctrlPressed) {
        return {SearchActionKind::NavigateFolderOnly, true};
    }
    if (mode == DialogMode::OpenFile) {
        return {
            shiftPressed ? SearchActionKind::NavigateFillAndConfirmOpen
                         : SearchActionKind::NavigateAndFillFileName,
            true
        };
    }
    // SaveFile / PickFolder / Unknown: 强制只跳父目录，绝不覆写文件名、绝不提交！
    return {SearchActionKind::NavigateFolderOnly, true};
}

/// 高分屏混合 DPI 缩放计算：对 DPI-Unaware 老程序使用 max(scaleForWindow, scaleForMonitor) 防止胶囊过小
inline float computeEffectiveDialogDpiScale(float windowScale, float monitorScale) noexcept {
    const float safeWin = std::clamp(windowScale, 0.75f, 4.0f);
    const float safeMon = std::clamp(monitorScale, 0.75f, 4.0f);
    return (std::max)(safeWin, safeMon);
}

inline bool isBottomDockPosition(std::string_view posConfig) noexcept {
    return posConfig == "bottom" || posConfig == "bottom-right" || posConfig == "bottom-center";
}

inline bool isCenterDockPosition(std::string_view posConfig) noexcept {
    return posConfig == "top-center" || posConfig == "bottom-center";
}

/// 兼容旧签名的水平安全定位计算（支持 top-right / top-center / bottom-right / bottom-center / top / bottom）
inline bool computeSafeRibbonHorizontalPlacement(
    const RECT& dlgRect,
    float dpiScale,
    int ribbonWidth,
    std::string_view posConfig,
    int& outPosX,
    int& outMaxAvailableWidth
) noexcept {
    const float safeScale = dpiScale > 0.1f ? dpiScale : 1.0f;
    const bool isBottom = isBottomDockPosition(posConfig);
    // 顶部停靠预留左上角图标+标题(112px*scale)与右上角三键(145px*scale)；
    // 底部停靠位于对话框外下缘，左右只需留 10px*scale 视觉边距。
    const int leftBound = dlgRect.left + static_cast<int>((isBottom ? 10.0f : 112.0f) * safeScale);
    const int rightBound = dlgRect.right - static_cast<int>((isBottom ? 10.0f : 145.0f) * safeScale);
    outMaxAvailableWidth = rightBound - leftBound;
    if (outMaxAvailableWidth < static_cast<int>(64.0f * safeScale)) {
        outPosX = leftBound;
        return false;
    }
    if (ribbonWidth <= 0 || ribbonWidth > outMaxAvailableWidth) {
        outPosX = leftBound;
        return false;
    }

    int posX = 0;
    if (isCenterDockPosition(posConfig)) {
        posX = dlgRect.left + ((dlgRect.right - dlgRect.left) - ribbonWidth) / 2;
    } else {
        posX = rightBound - ribbonWidth;
    }
    posX = (std::min)(posX, rightBound - ribbonWidth);
    posX = (std::max)(posX, leftBound);
    outPosX = posX;
    return true;
}

/// 计算包含垂直停靠（顶部标题栏内 vs 底部外缘，并自动钳制在屏幕工作区内）的完整定位
inline bool computeSafeRibbonPlacement(
    const RECT& frameRect,
    const RECT& monitorWorkArea,
    float dpiScale,
    int ribbonWidth,
    int ribbonHeight,
    std::string_view posConfig,
    int& outPosX,
    int& outPosY,
    int& outMaxAvailableWidth
) noexcept {
    if (!computeSafeRibbonHorizontalPlacement(
            frameRect, dpiScale, ribbonWidth, posConfig, outPosX, outMaxAvailableWidth)) {
        outPosY = frameRect.top;
        return false;
    }

    const float safeScale = dpiScale > 0.1f ? dpiScale : 1.0f;
    if (isBottomDockPosition(posConfig)) {
        const int gap = static_cast<int>(std::lround(4.0f * safeScale));
        int desiredY = frameRect.bottom + gap;
        // 如果贴在外下缘会超出工作区底边（例如对话框紧贴任务栏），则优雅内收至对话框底边内侧
        if (monitorWorkArea.bottom > monitorWorkArea.top &&
            desiredY + ribbonHeight > monitorWorkArea.bottom) {
            desiredY = (std::max)(
                static_cast<int>(monitorWorkArea.top),
                static_cast<int>(frameRect.bottom) - ribbonHeight - gap);
        }
        outPosY = desiredY;
    } else {
        outPosY = frameRect.top + static_cast<int>(std::lround(3.0f * safeScale));
    }
    return true;
}

/// 支持读取当前目录的文件管理器类型（按策略模式路由，Windows 资源管理器 + 主流第三方管理器）
enum class FileManagerKind : uint8_t {
    None = 0,
    WindowsExplorer = 1,   // CabinetWClass / ExploreWClass (Win10/Win11 多标签)
    TotalCommander = 2,    // TTOTAL_CMD
    DirectoryOpus = 3,     // dopus.lister
    Xyplorer = 4,          // XYplorer.exe
    QDir = 5,              // Q-Dir*.exe
    ModernThirdParty = 6,  // Files.exe / OneCommander.exe / DoubleCmd.exe / FreeCommander.exe / MultiCommander.exe
};

/// 可安全通过 UIA 导航的非标准文件对话框类型（纯 GPU 黑盒如 Blender 严禁匹配）
enum class NonStandardDialogKind : uint8_t {
    None = 0,
    QtFileDialog = 1,         // Qt5QWindow* / Qt6QWindow* (如 OBS / qBittorrent / WPS 部分弹窗)
    JetBrainsFileDialog = 2,  // SunAwtDialog (IDEA / PyCharm / WebStorm / CLion / GoLand / Rider 等)
};

inline std::wstring toLowerAsciiW(std::wstring_view raw) {
    std::wstring out;
    out.reserve(raw.size());
    for (wchar_t ch : raw) {
        out.push_back(foldUnicodeLowerInvariant(ch));
    }
    return out;
}

/// 纯函数识别支持的文件管理器窗口类型（结合 Win32 类名与可执行文件名，零磁盘 I/O）
inline FileManagerKind classifyFileManagerWindow(
    std::wstring_view className,
    std::wstring_view processName) noexcept {
    if (className == L"CabinetWClass" || className == L"ExploreWClass") {
        return FileManagerKind::WindowsExplorer;
    }
    if (className == L"TTOTAL_CMD") {
        return FileManagerKind::TotalCommander;
    }
    if (className == L"dopus.lister") {
        return FileManagerKind::DirectoryOpus;
    }

    auto ieq = [](std::wstring_view a, std::wstring_view b) noexcept -> bool {
        if (a.size() != b.size()) return false;
        for (size_t i = 0; i < a.size(); ++i) {
            if (foldUnicodeLowerInvariant(a[i]) != foldUnicodeLowerInvariant(b[i])) return false;
        }
        return true;
    };

    if (ieq(processName, L"xyplorer.exe")) {
        return FileManagerKind::Xyplorer;
    }
    if (ieq(processName, L"q-dir.exe") || ieq(processName, L"q-dir_x64.exe") || ieq(processName, L"q-dir_x86.exe")) {
        return FileManagerKind::QDir;
    }
    if (ieq(processName, L"totalcmd.exe") || ieq(processName, L"totalcmd64.exe")) {
        return FileManagerKind::TotalCommander;
    }
    if (ieq(processName, L"dopus.exe") && className.find(L"dopus") != std::wstring_view::npos) {
        return FileManagerKind::DirectoryOpus;
    }
    if (ieq(processName, L"files.exe") ||
        ieq(processName, L"onecommander.exe") ||
        ieq(processName, L"doublecmd.exe") ||
        ieq(processName, L"freecommander.exe") ||
        ieq(processName, L"multicommander.exe")) {
        return FileManagerKind::ModernThirdParty;
    }
    return FileManagerKind::None;
}

/// 纯函数识别可安全按需导航的非标准文件对话框候选（仅在用户显式按快捷键时做 UIA 二次校验）
inline NonStandardDialogKind classifyNonStandardDialogCandidate(
    std::wstring_view className,
    std::wstring_view processName,
    bool hasOwnerOrPopupStyle) noexcept {
    if (className.empty() || className == L"#32770") {
        return NonStandardDialogKind::None;
    }

    auto ieq = [](std::wstring_view a, std::wstring_view b) noexcept -> bool {
        if (a.size() != b.size()) return false;
        for (size_t i = 0; i < a.size(); ++i) {
            if (foldUnicodeLowerInvariant(a[i]) != foldUnicodeLowerInvariant(b[i])) return false;
        }
        return true;
    };

    if (className == L"SunAwtDialog") {
        if (ieq(processName, L"idea64.exe") || ieq(processName, L"idea.exe") ||
            ieq(processName, L"pycharm64.exe") || ieq(processName, L"pycharm.exe") ||
            ieq(processName, L"webstorm64.exe") || ieq(processName, L"webstorm.exe") ||
            ieq(processName, L"clion64.exe") || ieq(processName, L"clion.exe") ||
            ieq(processName, L"goland64.exe") || ieq(processName, L"goland.exe") ||
            ieq(processName, L"rider64.exe") || ieq(processName, L"rider.exe") ||
            ieq(processName, L"phpstorm64.exe") || ieq(processName, L"rubymine64.exe") ||
            ieq(processName, L"datagrip64.exe") || ieq(processName, L"studio64.exe")) {
            return NonStandardDialogKind::JetBrainsFileDialog;
        }
    }

    if (hasOwnerOrPopupStyle &&
        (className.rfind(L"Qt5QWindow", 0) == 0 || className.rfind(L"Qt6QWindow", 0) == 0)) {
        return NonStandardDialogKind::QtFileDialog;
    }

    return NonStandardDialogKind::None;
}

/// 纯函数从第三方文件管理器路径栏/标题栏原始文本中清洗并提取本地盘符或 UNC 目录路径
inline std::wstring extractDirectoryFromFileManagerText(
    FileManagerKind kind,
    std::wstring_view rawText) {
    if (rawText.empty()) return {};

    std::wstring s;
    s.reserve(rawText.size());
    for (wchar_t ch : rawText) {
        if (ch == 0x200E || ch == 0x200F || (ch >= 0x202A && ch <= 0x202E) || (ch >= 0x2066 && ch <= 0x2069)) {
            continue;
        }
        if (ch == L'\r' || ch == L'\n') break;
        s.push_back(ch);
    }

    auto trimSpaces = [](std::wstring& str) {
        while (!str.empty() && std::iswspace(str.front())) str.erase(str.begin());
        while (!str.empty() && std::iswspace(str.back())) str.pop_back();
    };
    trimSpaces(s);
    if (s.empty()) return {};

    // Total Commander TPathPanel 经典格式: "D:\Projects\Demo\*.*" 或 "D:\Projects\Demo\>"
    if (kind == FileManagerKind::TotalCommander) {
        while (!s.empty() && (s.back() == L'>' || std::iswspace(s.back()))) {
            s.pop_back();
        }
        if (s.size() >= 4 && s.compare(s.size() - 4, 4, L"\\*.*") == 0) {
            s.erase(s.size() - 4);
        } else if (s.size() >= 3 && s.compare(s.size() - 3, 3, L"*.*") == 0) {
            s.erase(s.size() - 3);
        } else if (s.size() >= 2 && s.compare(s.size() - 2, 2, L"\\*") == 0) {
            s.erase(s.size() - 2);
        }
    }

    // 剥离外部引号
    if (s.size() >= 2 && s.front() == L'"' && s.back() == L'"') {
        s = s.substr(1, s.size() - 2);
        trimSpaces(s);
    }

    // 若文本带前后装饰（如 "Address: D:\Work" 或 "D:\Work - XYplorer"），定位盘符或 UNC 起点
    size_t startPos = std::wstring::npos;
    for (size_t i = 0; i + 2 < s.size(); ++i) {
        if (std::iswalpha(s[i]) && s[i + 1] == L':' && (s[i + 2] == L'\\' || s[i + 2] == L'/')) {
            if (i == 0 || !std::iswalnum(s[i - 1])) {
                startPos = i;
                break;
            }
        }
    }
    if (startPos == std::wstring::npos) {
        const size_t uncPos = s.find(L"\\\\");
        if (uncPos != std::wstring::npos && uncPos + 3 < s.size() && s[uncPos + 2] != L'\\') {
            startPos = uncPos;
        }
    }
    if (startPos == std::wstring::npos) {
        // 兼容纯根盘符 "C:"
        if (s.size() == 2 && std::iswalpha(s[0]) && s[1] == L':') {
            return s + L"\\";
        }
        return {};
    }

    std::wstring candidate = s.substr(startPos);
    // 剥离常见窗体标题尾缀如 " - XYplorer" / " - Directory Opus" / " - OneCommander"
    for (const wchar_t* sep : {L" - XYplorer", L" - Directory Opus", L" - Total Commander",
                               L" - OneCommander", L" - Files", L" - FreeCommander", L" - Q-Dir"}) {
        const size_t pos = candidate.find(sep);
        if (pos != std::wstring::npos) {
            candidate.erase(pos);
        }
    }
    trimSpaces(candidate);

    for (wchar_t& ch : candidate) {
        if (ch == L'/') ch = L'\\';
    }
    while (candidate.size() > 3 && candidate.back() == L'\\') {
        candidate.pop_back();
    }
    if (candidate.size() == 2 && std::iswalpha(candidate[0]) && candidate[1] == L':') {
        candidate.push_back(L'\\');
    }
    return candidate;
}

}  // namespace tools3000::dialog
