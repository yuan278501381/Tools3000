/**
 * Tools3000 - High Performance Windows Productivity Suite
 *
 * Copyright (c) 2026 Yy1 (GitHub yuan278501381) <https://github.com/yuan278501381> & Tools3000 contributors
 *
 * Licensed under the GPL-3.0 License.
 */

#pragma once

#include "DialogEventAndLayoutPolicy.h"

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <windows.h>

namespace tools3000::dialog {

// 文件对话框架构兼容类型（保留供既有单测/调用点向后兼容，内部由 DialogArchitecture 映射）
enum class DialogType {
    Unknown,       // 未知
    Modern,        // IFileOpenDialog / IFileSaveDialog (Vista+ COM)
    Legacy,        // GetOpenFileName / OPENFILENAME (含 OFN_EXPLORER 与经典对话框)
    FolderPicker,  // 文件夹选择器 (SHBrowseForFolder)
};

// 单次 EnumChildWindows 采集的对话框完整结构与句柄快照（消除重复全树扫描）
struct DialogControlSnapshot {
    HWND dialogHwnd{nullptr};
    DialogArchitecture architecture{DialogArchitecture::Unknown};
    DialogMode mode{DialogMode::Unknown};
    DialogStructureSignals signals{};
    HWND addressEdit41477{nullptr};
    HWND breadcrumbParentHwnd{nullptr};
    HWND addressToolbarHwnd{nullptr};
    HWND fileNameEditHwnd{nullptr};
    HWND fileNameComboHwnd{nullptr};
    HWND fileTypeComboHwnd{nullptr};
    HWND shellViewHwnd{nullptr};
    HWND okButtonHwnd{nullptr};
    HWND cancelButtonHwnd{nullptr};
    HWND backButtonHwnd{nullptr};
    std::wstring okButtonText;
    std::wstring dialogTitle;
    bool isInstallerOrWizard{false};
    bool isProgressOrTransfer{false};
    bool isPropertySheet{false};
};

class DialogNavigator {
public:
    static DialogNavigator& instance();

    // 尝试将目标文件对话框导航至指定文件夹路径（全球化通用、零语言依赖、实测验证到位）
    bool navigateToFolder(HWND dialogHwnd, const std::string& folderPath);

    // 安全导航经 UIA 验证的非标准文件对话框（Qt QFileDialog / JetBrains SunAwtDialog），对纯 GPU 黑盒直接返回 false
    bool navigateNonStandardDialog(HWND dialogHwnd,
                                   NonStandardDialogKind kind,
                                   const std::string& folderPath);

    // 按对话框模式安全执行内嵌搜索结果跳转：
    // - 目录 / Ctrl+Enter / SaveFile / PickFolder / Unknown：强制仅跳目录，绝不改写文件名、绝不提交；
    // - OpenFile + 文件：先跳父目录并验证到位 -> 再填入文件名到 Edit 1152 -> Shift+Enter 才确认打开。
    bool navigateToSearchResult(HWND dialogHwnd,
                                const std::string& itemPath,
                                bool itemIsDirectory,
                                DialogMode mode,
                                bool ctrlPressed = false,
                                bool shiftPressed = false);

    // 异步投递目录导航至专用后台 MTA 工作线程（单槽覆盖旧导航，绝不误伤普通异步任务队列）
    void navigateToFolderAsync(HWND dialogHwnd,
                               const std::string& folderPath,
                               std::function<void(bool)> onComplete = {});

    // 异步投递内嵌搜索结果安全跳转至专用后台 MTA 工作线程
    void navigateToSearchResultAsync(HWND dialogHwnd,
                                     const std::string& itemPath,
                                     bool itemIsDirectory,
                                     DialogMode mode,
                                     bool ctrlPressed,
                                     bool shiftPressed,
                                     std::function<void(bool)> onComplete = {});

    // 异步投递通用后台探测/收藏任务至独立普通队列（不受导航取消/覆盖影响）
    void postAsyncTask(std::function<void()> task);

    // 启动/取消/停止专用后台 MTA 工作线程与受管 UNC 探测线程
    void startAsyncWorker();
    void cancelPendingNavigation();
    void shutdownAsyncWorker();

    // 可被取消纪元或引擎停机毫秒级中断的等待（替代硬 sleep_for）
    bool waitInterruptibleMs(uint32_t ms, uint64_t expectedNavEpoch = 0) const;

    // 单次 EnumChildWindows 采集完整结构与句柄快照
    static DialogControlSnapshot inspectDialog(HWND dialogHwnd, bool allowCurrentProcess = false);

    // 兼容检查：是否为有效文件打开/保存/文件夹选择对话框
    static bool isFileDialog(HWND hwnd, bool allowCurrentProcess = false);

    // 检查某个窗口句柄是否为文件传输/复制/移动/删除等进度操作弹窗
    static bool isProgressOrTransferDialog(HWND hwnd);

    // 检查指定进程名是否属于软件安装程序/卸载程序
    static bool isInstallerProcess(const std::string& processName);

    // 检查窗口或其宿主是否属于软件安装向导/卸载向导
    static bool isInstallerOrWizard(HWND dialogHwnd);

    // MECE 5 类架构与 3 模式精确检测
    static DialogArchitecture detectDialogArchitecture(HWND hwnd, bool allowCurrentProcess = true);
    static DialogMode detectDialogMode(HWND hwnd, bool allowCurrentProcess = true);
    static DialogType detectDialogType(HWND hwnd);

    // 将 MECE 5 类架构映射为兼容 DialogType
    static DialogType architectureToDialogType(DialogArchitecture arch) noexcept;

    // 查找核心控件句柄
    static HWND findPathEditControl(HWND dialogHwnd);
    static HWND findAddressBandControl(HWND dialogHwnd);
    static HWND findShellViewControl(HWND dialogHwnd);
    static HWND findOkButtonControl(HWND dialogHwnd);

    // 零 UIA 轻量化读取当前目录（专供 220ms 轮询使用：直接对缓存的 ToolbarWindow32 发一次 WM_GETTEXT）
    static std::string getCurrentDialogFolderFast(HWND dialogHwnd, HWND& cachedToolbarHwnd);

    // 实时提取对话框当前所在的真实文件夹绝对物理路径
    static std::string getCurrentDialogFolder(HWND dialogHwnd);

    // 实时提取用户当前选定/输入的目标项绝对物理路径（仅建议在关闭或 INVOKED 时调用）
    static std::string getSelectedPath(HWND dialogHwnd);

private:
    DialogNavigator() = default;
    ~DialogNavigator();

    struct UncProbeRequest {
        std::wstring path;
        std::shared_ptr<std::atomic<DWORD>> attrs;
        std::shared_ptr<std::atomic<bool>> done;
    };

    void asyncWorkerLoop();
    void probeWorkerLoop();
    void ensureProbeWorkerStarted();
    void shutdownProbeWorker();
    bool verifyNavigableDirectoryBounded(const std::wstring& candidate);

    bool navigateToFolderInternal(HWND dialogHwnd, const std::string& folderPath, uint64_t navEpoch);
    bool navigateModernDialog(HWND dialogHwnd,
                              const DialogControlSnapshot& snap,
                              const std::wstring& wPath,
                              uint64_t navEpoch);
    bool navigateLegacyDialog(HWND dialogHwnd,
                              const DialogControlSnapshot& snap,
                              const std::wstring& wPath,
                              uint64_t navEpoch);
    bool navigateToSearchResultInternal(HWND dialogHwnd,
                                        const std::string& itemPath,
                                        bool itemIsDirectory,
                                        DialogMode mode,
                                        bool ctrlPressed,
                                        bool shiftPressed,
                                        uint64_t navEpoch);

    // UIAutomation 辅助（仅在轻量化 Win32 读取失败或关闭取选中项时按需使用）
    static std::string uiaGetAddressBarPath(HWND dialogHwnd);
    static std::string uiaGetFileNameText(HWND dialogHwnd, bool* controlFound = nullptr);

    // 导航单槽与普通异步任务队列严格分离
    mutable std::mutex m_asyncMutex;
    mutable std::condition_variable m_asyncCv;
    std::thread m_asyncWorker;
    std::optional<std::function<void()>> m_pendingNavTask;
    std::deque<std::function<void()>> m_generalQueue;
    std::atomic<bool> m_asyncRunning{false};
    std::atomic<uint64_t> m_navCancelEpoch{1};

    // 受管单线程 UNC 异步探测槽（彻底消灭 std::thread::detach）
    std::mutex m_probeMutex;
    std::condition_variable m_probeCv;
    std::condition_variable m_probeDoneCv;
    std::thread m_probeWorker;
    std::optional<UncProbeRequest> m_probeRequest;
    std::atomic<bool> m_probeRunning{false};
};

} // namespace tools3000::dialog
