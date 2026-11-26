#pragma once
// ─────────────────────────────────────────────────────────────────────────────
// ClipboardManager.h — 原生 Win32 事件驱动剪贴板监听与 Rust 持久化桥接中枢
//
// 核心架构:
//   1. 0% CPU 闲置监听 (HWND_MESSAGE + AddClipboardFormatListener)；
//   2. 严格遵守 Windows 官方隐私标志 (ExcludeClipboardContentFromMonitorProcessing /
//      CanIncludeInClipboardHistory == 0 / Clipboard Viewer Ignore) + 进程名黑名单；
//   3. 微秒级读取剪贴板立即释放锁，图片编码/去重/持久化全部下沉至单后台工作线程，
//      0ms Ctrl+C 与 RDP 延迟；
//   4. 存储/检索/去重/置顶保护全部委托至纯 Rust 静态库 tools3000_clipboard_rs.lib。
// ─────────────────────────────────────────────────────────────────────────────

#ifndef TOOLS3000_CLIPBOARD_CLIPBOARDMANAGER_H
#define TOOLS3000_CLIPBOARD_CLIPBOARDMANAGER_H

#include <windows.h>
#include <nlohmann/json.hpp>

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <filesystem>
#include <functional>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace tools3000::clipboard {

struct SourceAppMetadata {
    std::string displayName;
    std::string iconDataUrl;
    std::string exePath;
};

struct ClipboardSettings {
    bool enabled = true;
    std::string hotkey = "Alt+V";
    int maxItems = 500;
    int maxDays = 30;
    int maxImageMb = 16;
    bool autoPaste = true;
    bool plainTextByDefault = false;
    bool captureImages = true;
    bool captureFiles = true;
    bool hidePinnedInOtherCategories = false;
    bool hideOnBlur = true;
    bool hideOnPaste = true;
    bool keepOpenAfterDrag = true;
    bool singleClickPaste = false;
    /// 窗口位置模式: "follow_mouse" (跟随光标所在显示器居中) | "remember_last" (记住上次位置与大小) | "fixed" (固定位置与大小)
    std::string windowPositionMode = "follow_mouse";
    int windowWidthDip = 760;
    int windowHeightDip = 560;
    bool hasFixedAnchor = false;
    int fixedX = 0;
    int fixedY = 0;
    double fixedRelX = 0.5;
    double fixedRelY = 0.38;
    std::string fixedMonitorDevice;
    bool lockWindowBounds = false;
    std::string excludedApps =
        "KeePass.exe,KeePassXC.exe,1Password.exe,Bitwarden.exe,Enpass.exe,LastPass.exe,RoboForm.exe";
};

class ClipboardManager final {
public:
    static ClipboardManager& instance();

    /// 主进程启动初始化（在 UI 线程调用，创建 HWND_MESSAGE 监听窗口并打开 Rust 存储）
    bool initialize(HINSTANCE hInstance, const std::filesystem::path& customDataDir = {});

    /// 优雅退出：注销剪贴板监听、排空后台图片编码线程、安全关闭 Rust 存储
    void shutdown();

    /// 读取与更新配置（支持热启停监听与自动淘汰过期记录）
    ClipboardSettings getSettings() const;
    bool updateSettings(const ClipboardSettings& next);
    void setEnabled(bool enabled);
    bool isEnabled() const noexcept { return m_enabled.load(std::memory_order_acquire); }

    /// 供 ClipboardWindow 与 SettingsPage 调用的高速 Rust 查询/操作接口
    nlohmann::json queryItems(const std::string& query, const std::string& kind,
                              uint32_t limit = 80, uint32_t offset = 0,
                              const std::string& groupId = {},
                              bool excludePinned = false) const;
    nlohmann::json getItem(uint64_t id) const;
    int togglePin(uint64_t id);
    bool deleteItem(uint64_t id);
    uint32_t deleteItems(const std::vector<uint64_t>& ids, bool skipPinned = false);
    bool updateItemText(uint64_t id, const std::string& newTextUtf8);
    uint32_t clearHistory(bool keepPinned);
    nlohmann::json getStats() const;

    /// 自定义收藏分组管理接口
    nlohmann::json listGroups() const;
    nlohmann::json createGroup(const std::string& nameUtf8);
    bool renameGroup(const std::string& groupId, const std::string& newNameUtf8);
    bool deleteGroup(const std::string& groupId);
    uint32_t setItemsGroup(const std::vector<uint64_t>& ids, const std::string& groupId);

    /// 将历史条目写回系统剪贴板，并可选向 targetHwnd 无感发送 Ctrl+V 粘贴（非纯文本模式原样保留 HTML/RTF 富文本）
    bool activateItem(uint64_t id, bool plainTextOnly, bool autoPaste, HWND targetHwnd = nullptr);

    /// 按选定顺序合并多条文本记录写回系统剪贴板并可选无感贴出
    bool activateItemsBatch(const std::vector<uint64_t>& ids, const std::string& separator,
                            bool autoPaste, HWND targetHwnd = nullptr);

    /// 返回图片历史条目的绝对文件路径（供一键贴图 / 离线 OCR / 原位翻译直连）
    std::filesystem::path resolveImageBlobPath(uint64_t id) const;
    std::filesystem::path getDataDir() const;
    std::filesystem::path getBlobsDir() const;

    /// 核对系统剪贴板序列号，若存在尚未捕获的新内容立即触发一次补采（微秒级无变更短路）
    void syncSystemClipboardIfNeeded();

    /// 注册内容变更通知回调（用于向独立悬浮面板推送实时更新，避免反向依赖 UI 层）
    void setChangeNotifier(std::function<void(const std::string&)> cb);

    /// 注册同进程目标窗口焦点恢复回调（如 SettingsWindow WebView2 焦点恢复，避免反向依赖 UI 层）
    void setTargetFocusRestorer(std::function<void(HWND)> cb);

    /// 登记来源应用的可执行文件路径并提取/缓存其友好名称与彩色原生图标（线程安全，供后台采集与测试调用）
    bool recordSourceAppMetadataForPath(const std::string& processExeName, const std::wstring& exePath);

    // ── 纯静态隐私策略、来源应用名称归一化与窗口目标校验工具函数（支持离线单元测试） ──
    static bool isExcludedProcessName(std::string_view processExeName, std::string_view excludedAppsCsv);
    static std::string normalizeProcessKey(std::string_view processExeName);
    static std::string normalizeFriendlyAppName(
        std::string_view fileDescription,
        std::string_view productName,
        std::string_view processExeName);
    static std::string fnv1a128Hex(const uint8_t* data, size_t len);
    static bool isTransientInternalOverlayClass(std::wstring_view className) noexcept;
    static HWND normalizeValidTargetHwnd(HWND candidate, HWND selfHwnd) noexcept;
    static HWND findFallbackTargetWindowBelow(HWND selfHwnd) noexcept;
    static bool isTargetForegroundActive(HWND targetHwnd) noexcept;

private:
    ClipboardManager() = default;
    ~ClipboardManager();
    ClipboardManager(const ClipboardManager&) = delete;
    ClipboardManager& operator=(const ClipboardManager&) = delete;

    struct PendingCaptureTask {
        uint8_t kind = 0; // 0=Text, 1=Image, 2=Files
        std::string textUtf8;
        std::vector<uint8_t> rawHtmlBytes;
        std::vector<uint8_t> rawRtfBytes;
        std::vector<uint8_t> rawPngBytes;
        std::vector<uint8_t> rawDibBytes;
        std::string sourceApp;
        std::wstring sourceExePath;
    };

    static LRESULT CALLBACK listenerWndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp);
    void ensureListenerRegistered(bool shouldListen);
    void handleClipboardUpdateDebounced();
    void enqueueTask(PendingCaptureTask&& task);
    void workerLoop(std::stop_token st);
    void processTask(PendingCaptureTask& task);

    bool writeTextToSystemClipboard(
        const std::wstring& wtext,
        const std::vector<uint8_t>& htmlBytes = {},
        const std::vector<uint8_t>& rtfBytes = {});
    bool writeFilesToSystemClipboard(const std::vector<std::wstring>& files);
    bool writePngImageToSystemClipboard(const std::filesystem::path& pngPath);
    std::filesystem::path resolveSafeBlobFile(const std::string& relName) const;
    void sendCtrlVPasteAsync(HWND targetHwnd);

    static constexpr UINT_PTR IDT_CLIP_DEBOUNCE = 4101;
    static constexpr UINT kDebounceMs = 45;
    static constexpr size_t kMaxRichTextBytes = 512u * 1024u; // 单条富文本 HTML/RTF 上限 512 KB

    mutable std::mutex m_settingsMutex;
    ClipboardSettings m_settings;
    std::filesystem::path m_dataDir;
    std::filesystem::path m_blobsDir;

    HWND m_listenerHwnd = nullptr;
    bool m_listenerRegistered = false;
    std::atomic<bool> m_initialized{false};
    std::atomic<bool> m_enabled{false};
    std::atomic<DWORD> m_suppressedSeq{0};
    DWORD m_lastProcessedSeq = 0;
    int m_openRetryCount = 0;
    static constexpr int kMaxOpenRetries = 5;

    // 注册 Windows 官方隐私与富文本/图片剪贴板格式 ID
    UINT m_fmtExcludeMonitor = 0;
    UINT m_fmtCanIncludeHistory = 0;
    UINT m_fmtViewerIgnore = 0;
    UINT m_fmtPng = 0;
    UINT m_fmtHtml = 0;
    UINT m_fmtRtf = 0;

    std::mutex m_queueMutex;
    std::condition_variable_any m_queueCv;
    std::deque<PendingCaptureTask> m_queue;
    std::jthread m_worker;

    mutable std::mutex m_notifierMutex;
    std::function<void(const std::string&)> m_changeNotifier;
    std::function<void(HWND)> m_targetFocusRestorer;
    void notifyChanged(const nlohmann::json& payload);

    void loadAppMetadataCacheLocked();
    void persistAppMetadataCacheLocked() const;
    nlohmann::json buildAppMetadataForItems(const nlohmann::json& items) const;

    static constexpr size_t kMaxCachedSourceApps = 256;
    mutable std::mutex m_appMetaMutex;
    mutable std::unordered_map<std::string, SourceAppMetadata> m_appMetaCache;
    mutable std::unordered_set<std::string> m_unresolvedAppKeys;
};

}  // namespace tools3000::clipboard

#endif  // TOOLS3000_CLIPBOARD_CLIPBOARDMANAGER_H
