/**
 * Tools3000 - High Performance Windows Productivity Suite
 *
 * Copyright (c) 2026 Yy1 (GitHub yuan278501381) <https://github.com/yuan278501381> & Tools3000 contributors
 *
 * Licensed under the GPL-3.0 License.
 */

#include "DialogRibbonOverlay.h"
#include "DialogEventAndLayoutPolicy.h"
#include "DialogNavigator.h"
#include "ExplorerTracker.h"
#include "PathMemoryManager.h"
#include "core/config/ConfigManager.h"
#include "core/events/MainThreadDispatcher.h"
#include "core/i18n/UiI18n.h"
#include "core/ipc/SearchPipeClient.h"
#include "core/logger/Logger.h"
#include "core/utils/DWriteFontUtils.h"
#include "core/utils/DpiUtils.h"
#include "core/utils/ThemeUtils.h"
#include "core/utils/WinUtils.h"

#include <commctrl.h>
#include <dwmapi.h>
#include <uxtheme.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <future>
#include <unordered_set>

#pragma comment(lib, "d2d1.lib")
#pragma comment(lib, "dwrite.lib")
#pragma comment(lib, "comctl32.lib")
#pragma comment(lib, "dwmapi.lib")
#pragma comment(lib, "uxtheme.lib")

extern HMODULE g_hDialogEnhancerModule;

using Microsoft::WRL::ComPtr;

namespace tools3000::dialog {

namespace {

const wchar_t* const RIBBON_WINDOW_CLASS = L"Tools3000_DialogRibbonOverlay";
const wchar_t* const SEARCH_POPUP_WINDOW_CLASS = L"Tools3000_DialogSearchPopup";

constexpr UINT WM_RIBBON_UPDATE_POS         = WM_USER + 101;
constexpr UINT WM_RIBBON_ATTACH             = WM_USER + 102;
constexpr UINT WM_RIBBON_HIDE               = WM_USER + 103;
constexpr UINT WM_RIBBON_THEME_CHANGED      = WM_USER + 104;
constexpr UINT WM_RIBBON_REFRESH_CONTENT    = WM_USER + 105;
constexpr UINT WM_RIBBON_TOGGLE_SEARCH      = WM_USER + 106;
constexpr UINT WM_SEARCH_RESULTS_READY      = WM_USER + 107;
constexpr UINT WM_SEARCH_RETRY_AFTER_WARMUP = WM_USER + 108;

constexpr int kInlineSearchHotkeyId         = 0x5431;
constexpr UINT_PTR kSearchDebounceTimerId   = 0x5432;
constexpr UINT_PTR kSearchEditSubclassId    = 0x5433;
constexpr UINT kSearchEditControlId         = 0x5434;
constexpr UINT kSearchDebounceMs            = 40;
constexpr int kMaxD2dConsecutiveFailures    = 3;
constexpr int kMaxVisibleSearchRows         = 8;

HINSTANCE resolveOverlayModuleInstance() noexcept {
    return g_hDialogEnhancerModule ? g_hDialogEnhancerModule : GetModuleHandleW(nullptr);
}

void configureSingleLineTrimming(IDWriteFactory* factory, IDWriteTextFormat* format) {
    if (!format) return;
    format->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
    DWRITE_TRIMMING trimming{};
    trimming.granularity = DWRITE_TRIMMING_GRANULARITY_CHARACTER;
    trimming.delimiter = 0;
    trimming.delimiterCount = 0;
    ComPtr<IDWriteInlineObject> ellipsis;
    if (factory && SUCCEEDED(factory->CreateEllipsisTrimmingSign(format, ellipsis.GetAddressOf())) && ellipsis) {
        format->SetTrimming(&trimming, ellipsis.Get());
    } else {
        format->SetTrimming(&trimming, nullptr);
    }
}

bool getTargetVisibleFrameBounds(HWND targetDialog, RECT& outRect) noexcept {
    if (!targetDialog || !IsWindow(targetDialog)) return false;
    // 优先使用 DWMWA_EXTENDED_FRAME_BOUNDS 精确剔除 Win10/11 不可见 7px 阴影外框
    if (SUCCEEDED(DwmGetWindowAttribute(
            targetDialog, DWMWA_EXTENDED_FRAME_BOUNDS, &outRect, sizeof(outRect))) &&
        outRect.right > outRect.left && outRect.bottom > outRect.top) {
        return true;
    }
    return GetWindowRect(targetDialog, &outRect) != FALSE;
}

std::string toLowerAsciiAndWideFold(const std::string& utf8) {
    std::wstring w = tools3000::core::WinUtils::utf8ToWstring(utf8);
    for (auto& ch : w) {
        ch = static_cast<wchar_t>(std::towlower(ch));
    }
    return tools3000::core::WinUtils::wstringToUtf8(w);
}

std::string normalizeDedupKey(const std::string& utf8Path) {
    std::wstring w = tools3000::core::WinUtils::utf8ToWstring(utf8Path);
    for (auto& ch : w) {
        if (ch == L'/') ch = L'\\';
        ch = static_cast<wchar_t>(std::towlower(ch));
    }
    while (w.size() > 3 && w.back() == L'\\') {
        w.pop_back();
    }
    return tools3000::core::WinUtils::wstringToUtf8(w);
}

bool matchesMultiTokenFolded(const std::string& haystackFolded, const std::vector<std::string>& tokens) {
    for (const auto& tok : tokens) {
        if (!tok.empty() && haystackFolded.find(tok) == std::string::npos) {
            return false;
        }
    }
    return true;
}

std::vector<std::string> splitFoldedTokens(const std::string& queryUtf8) {
    const std::string folded = toLowerAsciiAndWideFold(queryUtf8);
    std::vector<std::string> tokens;
    size_t i = 0;
    while (i < folded.size()) {
        while (i < folded.size() && (folded[i] == ' ' || folded[i] == '\t')) ++i;
        size_t start = i;
        while (i < folded.size() && folded[i] != ' ' && folded[i] != '\t') ++i;
        if (i > start) {
            std::string tok = folded.substr(start, i - start);
            // 本地候选均为文件夹，忽略用户手动键入的 folder:/dir: 前缀词
            if (tok == "folder:" || tok == "dir:") continue;
            if (tok.rfind("folder:", 0) == 0) tok.erase(0, 7);
            else if (tok.rfind("dir:", 0) == 0) tok.erase(0, 4);
            if (!tok.empty()) tokens.push_back(std::move(tok));
        }
    }
    return tokens;
}

InlineSearchEntry makeLocalEntry(const std::string& rawPath, InlineSearchSource source) {
    InlineSearchEntry entry;
    entry.path = rawPath;
    entry.isDirectory = true;
    entry.source = source;

    const std::filesystem::path fsPath = tools3000::core::WinUtils::utf8ToPath(rawPath).lexically_normal();
    std::wstring nameW = fsPath.filename().wstring();
    if (nameW.empty()) {
        nameW = fsPath.wstring();
    }
    entry.name = tools3000::core::WinUtils::wstringToUtf8(nameW);
    entry.parentDir = tools3000::core::WinUtils::pathToUtf8(fsPath.parent_path());
    if (entry.parentDir.empty()) {
        entry.parentDir = rawPath;
    }
    return entry;
}

}  // namespace

DialogRibbonOverlay& DialogRibbonOverlay::instance() {
    static DialogRibbonOverlay s_instance;
    return s_instance;
}

DialogRibbonOverlay::DialogRibbonOverlay() = default;

DialogRibbonOverlay::~DialogRibbonOverlay() {
    cleanup();
}

bool DialogRibbonOverlay::isOwnOverlayWindow(HWND hwnd) const noexcept {
    if (!hwnd) return false;
    std::lock_guard lock(m_mutex);
    if (hwnd == m_hwnd || hwnd == m_searchPopupHwnd || hwnd == m_searchEditHwnd || hwnd == m_tooltipHwnd) {
        return true;
    }
    HWND root = GetAncestor(hwnd, GA_ROOT);
    return root && (root == m_hwnd || root == m_searchPopupHwnd);
}

bool DialogRibbonOverlay::init() {
    {
        std::lock_guard lock(m_mutex);
        if (m_hwnd && IsWindow(m_hwnd)) return true;
    }

    if (tools3000::core::MainThreadDispatcher::instance().isInitialized() &&
        !tools3000::core::MainThreadDispatcher::instance().isOwnerThread()) {
        tools3000::core::MainThreadDispatcher::instance().post([this]() {
            init();
        });
        return false;
    }

    HINSTANCE hInst = resolveOverlayModuleInstance();

    WNDCLASSEXW wc{sizeof(WNDCLASSEXW)};
    wc.lpfnWndProc = WndProc;
    wc.hInstance = hInst;
    wc.lpszClassName = RIBBON_WINDOW_CLASS;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    RegisterClassExW(&wc);

    WNDCLASSEXW searchWc{sizeof(WNDCLASSEXW)};
    searchWc.lpfnWndProc = SearchPopupWndProc;
    searchWc.hInstance = hInst;
    searchWc.lpszClassName = SEARCH_POPUP_WINDOW_CLASS;
    searchWc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    RegisterClassExW(&searchWc);

    if (!m_d2dFactory) {
        D2D1CreateFactory(D2D1_FACTORY_TYPE_MULTI_THREADED, m_d2dFactory.GetAddressOf());
    }
    if (!m_dwriteFactory) {
        DWriteCreateFactory(
            DWRITE_FACTORY_TYPE_SHARED,
            __uuidof(IDWriteFactory),
            reinterpret_cast<IUnknown**>(m_dwriteFactory.GetAddressOf()));
    }
    ensureTextFormats(m_dpiScale);

    // P1 修复：严禁默认硬编码 WS_EX_TOPMOST！使用相对 Z-Order 动态贴合目标对话框。
    HWND createdHwnd = CreateWindowExW(
        WS_EX_LAYERED | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE,
        RIBBON_WINDOW_CLASS,
        L"Tools3000 Dialog Ribbon",
        WS_POPUP,
        0, 0, m_width, m_height,
        nullptr, nullptr, hInst, this);

    if (!createdHwnd) {
        LOG_ERROR("创建 DialogRibbonOverlay 窗口失败");
        return false;
    }

    {
        std::lock_guard lock(m_mutex);
        m_hwnd = createdHwnd;
    }

    INITCOMMONCONTROLSEX icex{sizeof(INITCOMMONCONTROLSEX), ICC_WIN95_CLASSES};
    InitCommonControlsEx(&icex);
    HWND tooltipHwnd = CreateWindowExW(
        WS_EX_TOOLWINDOW,
        TOOLTIPS_CLASSW,
        nullptr,
        WS_POPUP | TTS_ALWAYSTIP | TTS_NOPREFIX,
        CW_USEDEFAULT, CW_USEDEFAULT, CW_USEDEFAULT, CW_USEDEFAULT,
        createdHwnd, nullptr, hInst, nullptr);
    if (tooltipHwnd) {
        const bool isDark = tools3000::core::NativeThemePalette::isEffectiveDarkMode();
        SetWindowTheme(tooltipHwnd, isDark ? L"DarkMode_Explorer" : L"Explorer", nullptr);
        TTTOOLINFOW ti{};
        ti.cbSize = sizeof(TTTOOLINFOW);
        ti.uFlags = TTF_SUBCLASS;
        ti.hwnd = createdHwnd;
        ti.hinst = hInst;
        ti.uId = 1;
        ti.lpszText = const_cast<LPWSTR>(L"");
        ti.rect = {0, 0, 8192, 512};
        SendMessageW(tooltipHwnd, TTM_ADDTOOLW, 0, reinterpret_cast<LPARAM>(&ti));
        SendMessageW(tooltipHwnd, TTM_SETMAXTIPWIDTH, 0,
                     static_cast<LPARAM>(std::lround(420.0f * m_dpiScale)));
        SendMessageW(tooltipHwnd, TTM_SETDELAYTIME, TTDT_INITIAL, 220);
        m_tooltipHwnd = tooltipHwnd;
    }

    if (!m_memDC) {
        HDC screenDC = GetDC(nullptr);
        m_memDC = CreateCompatibleDC(screenDC);
        ReleaseDC(nullptr, screenDC);
    }

    return true;
}

void DialogRibbonOverlay::discardDeviceResources() {
    m_bgBrush.Reset();
    m_borderBrush.Reset();
    m_textBrush.Reset();
    m_textMutedBrush.Reset();
    m_highlightBrush.Reset();
    m_highlightBorderBrush.Reset();
    m_syncBgBrush.Reset();
    m_syncBorderBrush.Reset();
    m_chevronBrush.Reset();
    m_badgeBgBrush.Reset();
    m_badgeTextBrush.Reset();
    m_dividerBrush.Reset();
    m_whiteBrush.Reset();
    m_brushesValid = false;
    m_renderTarget.Reset();
}

void DialogRibbonOverlay::discardSearchDeviceResources() {
    m_searchRenderTarget.Reset();
}

void DialogRibbonOverlay::cleanup() {
    // 先停掉后台搜索线程并取消本插件发起的在途管道查询（零阻塞等待）
    stopSearchWorker();

    auto& disp = tools3000::core::MainThreadDispatcher::instance();
    if (disp.isInitialized() && !disp.isOwnerThread()) {
        // P0 热卸载防护：非 UI 线程必须同步等待 UI 线程完成 DestroyWindow 与 UnregisterClassW，
        // 严禁异步投递后立即 FreeLibrary 导致幽灵 WndProc 崩溃！（带防死锁超时保护）
        auto done = std::make_shared<std::promise<void>>();
        auto fut = done->get_future();
        if (disp.post([this, done]() {
                try {
                    cleanupOnUiThread();
                } catch (...) {
                }
                try {
                    done->set_value();
                } catch (...) {
                }
            })) {
            (void)fut.wait_for(std::chrono::milliseconds(3000));
            return;
        }
    }

    cleanupOnUiThread();
}

void DialogRibbonOverlay::cleanupOnUiThread() {
    syncInlineSearchHotkey(false);

    HWND hwndToDestroy = nullptr;
    HWND tooltipToDestroy = nullptr;
    HWND searchPopupToDestroy = nullptr;
    HWND searchEditToDestroy = nullptr;
    {
        std::lock_guard lock(m_mutex);
        hwndToDestroy = m_hwnd;
        tooltipToDestroy = m_tooltipHwnd;
        searchPopupToDestroy = m_searchPopupHwnd;
        searchEditToDestroy = m_searchEditHwnd;
        m_hwnd = nullptr;
        m_tooltipHwnd = nullptr;
        m_searchPopupHwnd = nullptr;
        m_searchEditHwnd = nullptr;
        m_searchOpen = false;
        m_currentTooltipText.clear();
        m_posUpdatePending.store(false, std::memory_order_release);
        m_dismissedDialog = nullptr;
        m_cachedFolderOwner = nullptr;
        resetStateLocked();
    }

    if (searchEditToDestroy && IsWindow(searchEditToDestroy)) {
        RemoveWindowSubclass(searchEditToDestroy, SearchEditSubclassProc, kSearchEditSubclassId);
        DestroyWindow(searchEditToDestroy);
    }
    if (searchPopupToDestroy && IsWindow(searchPopupToDestroy)) {
        KillTimer(searchPopupToDestroy, kSearchDebounceTimerId);
        DestroyWindow(searchPopupToDestroy);
    }
    if (tooltipToDestroy && IsWindow(tooltipToDestroy)) {
        DestroyWindow(tooltipToDestroy);
    }
    if (hwndToDestroy && IsWindow(hwndToDestroy)) {
        DestroyWindow(hwndToDestroy);
    }

    if (m_searchEditFont) {
        DeleteObject(m_searchEditFont);
        m_searchEditFont = nullptr;
    }
    if (m_searchEditBgBrush) {
        DeleteObject(m_searchEditBgBrush);
        m_searchEditBgBrush = nullptr;
    }

    m_vdm.Reset();
    m_vdmInitialized = false;
    if (m_memDC) {
        if (m_oldBitmap) {
            SelectObject(m_memDC, m_oldBitmap);
            m_oldBitmap = nullptr;
        }
        if (m_memBitmap) {
            DeleteObject(m_memBitmap);
            m_memBitmap = nullptr;
        }
        DeleteDC(m_memDC);
        m_memDC = nullptr;
    }
    discardDeviceResources();
    discardSearchDeviceResources();
    m_textFormat.Reset();
    m_boldFormat.Reset();
    m_badgeFormat.Reset();
    m_searchTitleFormat.Reset();
    m_searchPathFormat.Reset();
    m_searchMetaFormat.Reset();
    m_dwriteFactory.Reset();
    m_d2dFactory.Reset();

    // P0 热卸载关键：销毁全部窗口后，使用与 RegisterClassExW 完全成对的模块句柄注销窗口类！
    HINSTANCE hInst = resolveOverlayModuleInstance();
    UnregisterClassW(SEARCH_POPUP_WINDOW_CLASS, hInst);
    UnregisterClassW(RIBBON_WINDOW_CLASS, hInst);

    // 仅在真正冷退场清理路径调用物理内存修剪
    tools3000::core::WinUtils::trimWorkingSet();
}

void DialogRibbonOverlay::setCachedDialogFolder(HWND dialogHwnd, const std::string& folderPath) {
    if (!dialogHwnd || folderPath.empty()) return;
    std::lock_guard lock(m_mutex);
    if (m_targetDialog == nullptr || m_targetDialog == dialogHwnd || m_cachedFolderOwner == dialogHwnd) {
        m_cachedFolderOwner = dialogHwnd;
        m_cachedDialogFolder = folderPath;
    }
}

void DialogRibbonOverlay::attachValidatedOnUiThread(HWND dialogHwnd,
                                                    DWORD processId,
                                                    const std::string& processName,
                                                    DialogArchitecture architecture,
                                                    DialogMode mode,
                                                    const std::string& initialFolder) {
    if (!dialogHwnd || !IsWindow(dialogHwnd) || processId == 0) return;
    if (!PathMemoryManager::instance().isRibbonEnabled()) return;

    {
        std::lock_guard lock(m_mutex);
        if (m_dismissedDialog == dialogHwnd) {
            return;
        }
    }

    HWND hwnd = getHwnd();
    if (!hwnd || !IsWindow(hwnd)) {
        if (!init()) return;
        hwnd = getHwnd();
    }

    {
        std::lock_guard lock(m_mutex);
        // P1 修复：绝不因 m_targetDialog 刚切换就把同一 dialogHwnd 已写入的 m_cachedDialogFolder 清空！
        if (m_cachedFolderOwner != dialogHwnd) {
            m_cachedFolderOwner = dialogHwnd;
            m_cachedDialogFolder = initialFolder;
        } else if (!initialFolder.empty()) {
            m_cachedDialogFolder = initialFolder;
        }
        m_targetDialog = dialogHwnd;
        m_targetProcessId = processId;
        m_targetProcess = processName;
        if (architecture != DialogArchitecture::Unknown) {
            m_targetArchitecture = architecture;
        }
        if (mode != DialogMode::Unknown) {
            m_targetDialogMode = mode;
        }
        m_d2dConsecutiveFailures = 0;
        m_d2dRetryAfterTick = 0;
    }

    ExplorerTracker::instance().requestRefresh();

    if (hwnd && IsWindow(hwnd)) {
        PostMessageW(hwnd, WM_RIBBON_ATTACH, 0, 0);
    }
}

void DialogRibbonOverlay::attachToDialog(HWND dialogHwnd, const std::string& processName) {
    attachToDialog(dialogHwnd, processName, DialogArchitecture::Unknown, DialogMode::Unknown, "");
}

void DialogRibbonOverlay::attachToDialog(HWND dialogHwnd,
                                         const std::string& processName,
                                         DialogArchitecture architecture,
                                         DialogMode mode,
                                         const std::string& initialFolder) {
    if (!dialogHwnd || !IsWindow(dialogHwnd)) return;
    if (!PathMemoryManager::instance().isRibbonEnabled()) return;

    wchar_t className[64]{};
    if (GetClassNameW(dialogHwnd, className, static_cast<int>(std::size(className))) <= 0 ||
        wcscmp(className, L"#32770") != 0) {
        return;
    }

    DWORD processId = 0;
    GetWindowThreadProcessId(dialogHwnd, &processId);
    if (processId == 0) return;

    auto& disp = tools3000::core::MainThreadDispatcher::instance();
    if (architecture == DialogArchitecture::Unknown && disp.isInitialized() && disp.isOwnerThread()) {
        // 严禁在主 UI 线程直接做跨进程 inspectDialog
        DialogNavigator::instance().postAsyncTask([this, dialogHwnd, processId, processName, initialFolder]() {
            if (!IsWindow(dialogHwnd)) return;
            const DialogControlSnapshot snap = DialogNavigator::inspectDialog(dialogHwnd, false);
            if (snap.isProgressOrTransfer || snap.isPropertySheet ||
                snap.architecture == DialogArchitecture::Unknown) {
                return;
            }
            std::string folder = initialFolder;
            if (folder.empty()) {
                HWND tb = snap.addressToolbarHwnd;
                folder = DialogNavigator::getCurrentDialogFolderFast(dialogHwnd, tb);
                if (folder.empty()) {
                    folder = DialogNavigator::getCurrentDialogFolder(dialogHwnd);
                }
            }
            tools3000::core::MainThreadDispatcher::instance().post(
                [this, dialogHwnd, processId, processName, arch = snap.architecture, m = snap.mode, folder]() {
                    attachValidatedOnUiThread(dialogHwnd, processId, processName, arch, m, folder);
                });
        });
        return;
    }

    if (architecture == DialogArchitecture::Unknown) {
        const DialogControlSnapshot snap = DialogNavigator::inspectDialog(dialogHwnd, false);
        if (snap.isProgressOrTransfer || snap.isPropertySheet ||
            snap.architecture == DialogArchitecture::Unknown) {
            return;
        }
        architecture = snap.architecture;
        mode = snap.mode;
    }

    if (disp.isInitialized() && !disp.isOwnerThread()) {
        disp.post([this, dialogHwnd, processId, processName, architecture, mode, initialFolder]() {
            attachValidatedOnUiThread(dialogHwnd, processId, processName, architecture, mode, initialFolder);
        });
        return;
    }

    attachValidatedOnUiThread(dialogHwnd, processId, processName, architecture, mode, initialFolder);
}

void DialogRibbonOverlay::updatePosition() {
    const HWND hwnd = getHwnd();
    if (hwnd && IsWindow(hwnd)) {
        if (!m_posUpdatePending.exchange(true, std::memory_order_acq_rel)) {
            if (!PostMessageW(hwnd, WM_RIBBON_UPDATE_POS, 0, 0)) {
                m_posUpdatePending.store(false, std::memory_order_release);
            }
        }
    }
}

void DialogRibbonOverlay::refreshContent(bool clearDismissed) {
    if (clearDismissed) {
        std::lock_guard lock(m_mutex);
        m_dismissedDialog = nullptr;
    }
    const HWND hwnd = getHwnd();
    if (hwnd && IsWindow(hwnd)) {
        PostMessageW(hwnd, WM_RIBBON_REFRESH_CONTENT, clearDismissed ? 1 : 0, 0);
    }
}

void DialogRibbonOverlay::onThemeChanged() {
    const HWND hwnd = getHwnd();
    if (hwnd && IsWindow(hwnd)) {
        PostMessageW(hwnd, WM_RIBBON_THEME_CHANGED, 0, 0);
    }
}

void DialogRibbonOverlay::hide() {
    const HWND hwnd = getHwnd();
    if (hwnd && IsWindow(hwnd)) {
        PostMessageW(hwnd, WM_RIBBON_HIDE, 0, 0);
    }
}

void DialogRibbonOverlay::toggleInlineSearch() {
    const HWND hwnd = getHwnd();
    if (hwnd && IsWindow(hwnd)) {
        PostMessageW(hwnd, WM_RIBBON_TOGGLE_SEARCH, 0, 0);
    }
}

void DialogRibbonOverlay::syncInlineSearchHotkey(bool wantRegistered) {
    HWND hwnd = nullptr;
    {
        std::lock_guard lock(m_mutex);
        hwnd = m_hwnd;
    }
    if (!hwnd || !IsWindow(hwnd)) {
        m_hotkeyRegistered = false;
        return;
    }
    if (wantRegistered && !m_hotkeyRegistered) {
        if (RegisterHotKey(hwnd, kInlineSearchHotkeyId, MOD_CONTROL | MOD_NOREPEAT, 'G')) {
            m_hotkeyRegistered = true;
        }
    } else if (!wantRegistered && m_hotkeyRegistered) {
        UnregisterHotKey(hwnd, kInlineSearchHotkeyId);
        m_hotkeyRegistered = false;
    }
}

void DialogRibbonOverlay::applyRelativeZOrder(
    HWND overlayHwnd, HWND targetDialog, int x, int y, int w, int h, UINT extraFlags) {
    if (!overlayHwnd || !IsWindow(overlayHwnd) || !targetDialog || !IsWindow(targetDialog)) return;

    const bool targetTopmost = (GetWindowLongPtrW(targetDialog, GWL_EXSTYLE) & WS_EX_TOPMOST) != 0;
    const bool overlayTopmost = (GetWindowLongPtrW(overlayHwnd, GWL_EXSTYLE) & WS_EX_TOPMOST) != 0;

    HWND insertAfter = HWND_TOP;
    if (targetTopmost) {
        insertAfter = HWND_TOPMOST;
    } else {
        if (overlayTopmost) {
            SetWindowPos(overlayHwnd, HWND_NOTOPMOST, 0, 0, 0, 0,
                         SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_NOREDRAW);
        }
        HWND prev = GetWindow(targetDialog, GW_HWNDPREV);
        while (prev && isOwnOverlayWindow(prev)) {
            prev = GetWindow(prev, GW_HWNDPREV);
        }
        insertAfter = prev ? prev : HWND_TOP;
    }

    SetWindowPos(overlayHwnd, insertAfter, x, y, w, h, SWP_NOACTIVATE | extraFlags);
}

void DialogRibbonOverlay::doAttachToDialog() {
    {
        std::lock_guard lock(m_mutex);
        m_buttons.clear();
        m_renderedWidth = 0;
        m_renderedHeight = 0;
        m_d2dConsecutiveFailures = 0;
        m_d2dRetryAfterTick = 0;
    }
    doUpdatePosition();
}

void DialogRibbonOverlay::doHide() {
    closeInlineSearchPopup(false);
    syncInlineSearchHotkey(false);
    HWND hwnd = nullptr;
    {
        std::lock_guard lock(m_mutex);
        hwnd = m_hwnd;
        resetStateLocked();
    }
    updateTooltipText("");
    if (hwnd && IsWindow(hwnd)) {
        ShowWindow(hwnd, SW_HIDE);
    }
}

void DialogRibbonOverlay::resetStateLocked() {
    m_targetDialog = nullptr;
    m_targetProcessId = 0;
    m_targetArchitecture = DialogArchitecture::Unknown;
    m_targetDialogMode = DialogMode::Unknown;
    m_targetProcess.clear();
    m_cachedFolderOwner = nullptr;
    m_cachedDialogFolder.clear();
    m_activeExplorerPath.clear();
    m_buttons.clear();
    m_renderedWidth = 0;
    m_renderedHeight = 0;
    m_lastMaxAvailableWidth = 0;
    m_menuOpen = false;
}

bool DialogRibbonOverlay::computeSafeHorizontalPlacement(
    const RECT& dlgRect,
    float dpiScale,
    int ribbonWidth,
    const std::string& posConfig,
    int& outPosX,
    int& outMaxAvailableWidth
) noexcept {
    return computeSafeRibbonHorizontalPlacement(
        dlgRect, dpiScale, ribbonWidth, posConfig, outPosX, outMaxAvailableWidth);
}

bool DialogRibbonOverlay::doUpdatePosition() {
    HWND hwnd = nullptr;
    HWND targetDialog = nullptr;
    HWND dismissedDialog = nullptr;
    DWORD targetProcessId = 0;
    std::string targetProcess;
    bool menuOpen = false;
    bool needFullRebuild = false;
    float oldDpiScale = 1.0f;
    tools3000::core::UiLanguage oldLang = tools3000::core::UiLanguage::ZhCN;
    int oldMaxAvailableWidth = 0;

    {
        std::lock_guard lock(m_mutex);
        hwnd = m_hwnd;
        targetDialog = m_targetDialog;
        dismissedDialog = m_dismissedDialog;
        targetProcessId = m_targetProcessId;
        targetProcess = m_targetProcess;
        menuOpen = m_menuOpen;
        needFullRebuild = m_buttons.empty() || m_renderedWidth <= 0;
        oldDpiScale = m_dpiScale;
        oldLang = m_currentUiLang;
        oldMaxAvailableWidth = m_lastMaxAvailableWidth;
    }

    try {
        if (!targetDialog || targetDialog == dismissedDialog || !IsWindow(targetDialog) ||
            !hwnd || !IsWindow(hwnd) || targetProcessId == 0) {
            doHide();
            return false;
        }

        wchar_t className[64]{};
        DWORD currentProcessId = 0;
        GetWindowThreadProcessId(targetDialog, &currentProcessId);
        if (currentProcessId != targetProcessId ||
            GetClassNameW(targetDialog, className, static_cast<int>(std::size(className))) <= 0 ||
            wcscmp(className, L"#32770") != 0) {
            doHide();
            return false;
        }

        if (needFullRebuild && !targetProcess.empty()) {
            const std::string currentProcess =
                tools3000::core::WinUtils::getProcessNameFromWindow(targetDialog);
            if (currentProcess.empty() ||
                _stricmp(currentProcess.c_str(), targetProcess.c_str()) != 0) {
                doHide();
                return false;
            }
        }

        if (menuOpen) {
            return true;
        }

        if (IsIconic(targetDialog) || !IsWindowVisible(targetDialog)) {
            closeInlineSearchPopup(false);
            syncInlineSearchHotkey(false);
            if (IsWindowVisible(hwnd)) ShowWindow(hwnd, SW_HIDE);
            return false;
        }

        // 前台归属硬校验：同时认可目标对话框、主胶囊、内嵌搜索浮层及其 Edit 子控件！
        const HWND foreground = GetForegroundWindow();
        const HWND foregroundRoot = foreground ? GetAncestor(foreground, GA_ROOT) : nullptr;
        const bool isTargetOrOwnForeground =
            (foreground == targetDialog || foregroundRoot == targetDialog ||
             isOwnOverlayWindow(foreground) || isOwnOverlayWindow(foregroundRoot));
        if (!isTargetOrOwnForeground) {
            closeInlineSearchPopup(false);
            syncInlineSearchHotkey(false);
            if (IsWindowVisible(hwnd)) ShowWindow(hwnd, SW_HIDE);
            return false;
        }

        // 虚拟桌面归属校验
        if (!m_vdmInitialized) {
            m_vdmInitialized = true;
            constexpr CLSID kClsidVirtualDesktopManager = {
                0xaa509086, 0x5ca9, 0x4c25, {0x8f, 0x95, 0x58, 0x9d, 0x3c, 0x07, 0xb4, 0x8a}};
            CoCreateInstance(kClsidVirtualDesktopManager, nullptr, CLSCTX_ALL,
                             IID_PPV_ARGS(m_vdm.GetAddressOf()));
        }
        if (m_vdm) {
            BOOL onCurrentDesktop = TRUE;
            if (SUCCEEDED(m_vdm->IsWindowOnCurrentVirtualDesktop(targetDialog, &onCurrentDesktop)) &&
                !onCurrentDesktop) {
                closeInlineSearchPopup(false);
                syncInlineSearchHotkey(false);
                if (IsWindowVisible(hwnd)) ShowWindow(hwnd, SW_HIDE);
                return false;
            }
        }

        // 仅在活动对话框处于前台且开启内嵌搜索时动态注册 Ctrl+G，零全局污染
        syncInlineSearchHotkey(PathMemoryManager::instance().isInlineSearchEnabled());

        RECT frameRect{};
        if (!getTargetVisibleFrameBounds(targetDialog, frameRect)) {
            doHide();
            return false;
        }

        const HMONITOR monitor = MonitorFromWindow(targetDialog, MONITOR_DEFAULTTONEAREST);
        const RECT monWorkArea = tools3000::core::dpi::workArea(monitor);
        const float winScale = tools3000::core::dpi::scaleForWindow(targetDialog);
        const float monScale = tools3000::core::dpi::scaleForMonitor(monitor);
        const float newDpiScale = computeEffectiveDialogDpiScale(winScale, monScale);

        const tools3000::core::UiLanguage curLang = tools3000::core::UiI18n::getEffectiveLanguage();
        const bool dpiChanged = std::abs(newDpiScale - oldDpiScale) > 0.001f;
        const bool langChanged = (curLang != oldLang);

        int probePosX = 0;
        int maxAvailableWidth = 0;
        const std::string posConfig = PathMemoryManager::instance().getRibbonPosition();
        computeSafeHorizontalPlacement(frameRect, newDpiScale, 64, posConfig, probePosX, maxAvailableWidth);
        if (maxAvailableWidth < static_cast<int>(64.0f * newDpiScale)) {
            if (IsWindowVisible(hwnd)) ShowWindow(hwnd, SW_HIDE);
            return false;
        }

        const bool availChanged = std::abs(maxAvailableWidth - oldMaxAvailableWidth) > 1;
        const bool layoutRebuilt = needFullRebuild || dpiChanged || langChanged || availChanged;

        int winWidth = 0;
        int winHeight = 0;
        int posX = 0;
        int posY = 0;
        bool needRender = false;

        {
            std::lock_guard lock(m_mutex);
            if (m_targetDialog != targetDialog) {
                return false;
            }
            m_dpiScale = newDpiScale;
            if (layoutRebuilt) {
                m_lastMaxAvailableWidth = maxAvailableWidth;
                updateButtonsLayout(newDpiScale, maxAvailableWidth);
            }

            if (m_buttons.empty() ||
                !computeSafeRibbonPlacement(frameRect, monWorkArea, newDpiScale,
                                            m_width, m_height, posConfig,
                                            posX, posY, maxAvailableWidth)) {
                winWidth = 0;
            } else {
                m_posX = posX;
                m_posY = posY;
                winWidth = m_width;
                winHeight = m_height;
                needRender = layoutRebuilt || m_renderedWidth != m_width || m_renderedHeight != m_height;
            }
        }

        if (winWidth <= 0 || winHeight <= 0) {
            if (IsWindowVisible(hwnd)) ShowWindow(hwnd, SW_HIDE);
            return false;
        }

        const UINT swpFlags = needRender ? 0u : SWP_NOREDRAW;
        applyRelativeZOrder(hwnd, targetDialog, posX, posY, winWidth, winHeight, swpFlags);
        if (needRender) {
            if (m_tooltipHwnd && IsWindow(m_tooltipHwnd)) {
                SendMessageW(m_tooltipHwnd, TTM_SETMAXTIPWIDTH, 0,
                             static_cast<LPARAM>(std::lround(420.0f * newDpiScale)));
                TTTOOLINFOW ti{};
                ti.cbSize = sizeof(TTTOOLINFOW);
                ti.hwnd = hwnd;
                ti.uId = 1;
                ti.rect = {0, 0, winWidth, winHeight};
                SendMessageW(m_tooltipHwnd, TTM_NEWTOOLRECTW, 0, reinterpret_cast<LPARAM>(&ti));
            }
            render();
        }
        if (!IsWindowVisible(hwnd)) {
            ShowWindow(hwnd, SW_SHOWNOACTIVATE);
        }
        if (m_searchOpen && m_searchPopupHwnd && IsWindow(m_searchPopupHwnd)) {
            layoutSearchPopup();
        }
        return true;
    } catch (const std::exception& e) {
        LOG_WARN("doUpdatePosition 异常: {}", e.what());
    } catch (...) {
        LOG_WARN("doUpdatePosition 未知异常");
    }
    doHide();
    return false;
}

void DialogRibbonOverlay::ensureTextFormats(float dpiScale) {
    const tools3000::core::UiLanguage lang = tools3000::core::UiI18n::getEffectiveLanguage();
    if (m_dwriteFactory &&
        (m_textFormat == nullptr || std::abs(m_currentFontDpi - dpiScale) > 0.001f || m_currentUiLang != lang)) {
        m_textFormat.Reset();
        m_boldFormat.Reset();
        m_badgeFormat.Reset();
        m_searchTitleFormat.Reset();
        m_searchPathFormat.Reset();
        m_searchMetaFormat.Reset();
        m_currentFontDpi = dpiScale;
        m_currentUiLang = lang;

        const wchar_t* localeName = tools3000::core::resolveInstalledUiLocaleNameW();
        const wchar_t* fontName = tools3000::core::resolveInstalledUiFontFamily(m_dwriteFactory.Get());

        const float fontSize = 12.0f * dpiScale;
        const float titleSize = 13.0f * dpiScale;
        const float pathSize = 11.8f * dpiScale;
        const float metaSize = 11.8f * dpiScale;

        auto makeFmt = [&](DWRITE_FONT_WEIGHT weight, float sz, ComPtr<IDWriteTextFormat>& out) {
            if (SUCCEEDED(m_dwriteFactory->CreateTextFormat(
                    fontName, nullptr, weight, DWRITE_FONT_STYLE_NORMAL,
                    DWRITE_FONT_STRETCH_NORMAL, sz, localeName, out.GetAddressOf())) && out) {
                configureSingleLineTrimming(m_dwriteFactory.Get(), out.Get());
            }
        };

        makeFmt(DWRITE_FONT_WEIGHT_MEDIUM, fontSize, m_textFormat);
        makeFmt(DWRITE_FONT_WEIGHT_SEMI_BOLD, fontSize, m_boldFormat);
        makeFmt(DWRITE_FONT_WEIGHT_BOLD, fontSize, m_badgeFormat);
        makeFmt(DWRITE_FONT_WEIGHT_SEMI_BOLD, titleSize, m_searchTitleFormat);
        makeFmt(DWRITE_FONT_WEIGHT_MEDIUM, pathSize, m_searchPathFormat);
        makeFmt(DWRITE_FONT_WEIGHT_MEDIUM, metaSize, m_searchMetaFormat);
    }
}

// ============================================================
// 高保真纯矢量图标绘制
// ============================================================
void DialogRibbonOverlay::drawSearchIcon(ID2D1RenderTarget* rt, ID2D1Brush* brush, float x, float y, float size, float strokeWidth) {
    if (!rt || !brush) return;
    const float r = size * 0.34f;
    const float cx = x + r + strokeWidth * 0.5f;
    const float cy = y + r + strokeWidth * 0.5f;
    rt->DrawEllipse(D2D1::Ellipse(D2D1::Point2F(cx, cy), r, r), brush, strokeWidth);
    const float handleStart = r * 0.72f;
    rt->DrawLine(
        D2D1::Point2F(cx + handleStart, cy + handleStart),
        D2D1::Point2F(x + size, y + size),
        brush, strokeWidth * 1.15f);
}

void DialogRibbonOverlay::drawFolderIcon(ID2D1RenderTarget* rt, ID2D1Brush* brush, float x, float y, float size, float strokeWidth) {
    if (!rt || !brush) return;
    const float tabW = size * 0.42f;
    const float tabH = size * 0.28f;
    const float corner = size * 0.15f;

    D2D1_RECT_F bodyRect = D2D1::RectF(x, y + tabH * 0.7f, x + size, y + size);
    rt->DrawRoundedRectangle(D2D1::RoundedRect(bodyRect, corner, corner), brush, strokeWidth);
    rt->DrawLine(D2D1::Point2F(x + corner, y + tabH * 0.7f), D2D1::Point2F(x + corner, y + corner), brush, strokeWidth);
    rt->DrawLine(D2D1::Point2F(x + corner, y + corner), D2D1::Point2F(x + tabW, y + corner), brush, strokeWidth);
    rt->DrawLine(D2D1::Point2F(x + tabW, y + corner), D2D1::Point2F(x + tabW + tabH * 0.5f, y + tabH * 0.7f), brush, strokeWidth);
}

void DialogRibbonOverlay::drawFileIcon(ID2D1RenderTarget* rt, ID2D1Brush* brush, float x, float y, float size, float strokeWidth) {
    if (!rt || !brush) return;
    const float w = size * 0.78f;
    const float left = x + (size - w) * 0.5f;
    const float right = left + w;
    const float bottom = y + size;
    const float fold = size * 0.28f;

    rt->DrawLine(D2D1::Point2F(left, y), D2D1::Point2F(right - fold, y), brush, strokeWidth);
    rt->DrawLine(D2D1::Point2F(right - fold, y), D2D1::Point2F(right, y + fold), brush, strokeWidth);
    rt->DrawLine(D2D1::Point2F(right, y + fold), D2D1::Point2F(right, bottom), brush, strokeWidth);
    rt->DrawLine(D2D1::Point2F(right, bottom), D2D1::Point2F(left, bottom), brush, strokeWidth);
    rt->DrawLine(D2D1::Point2F(left, bottom), D2D1::Point2F(left, y), brush, strokeWidth);
    rt->DrawLine(D2D1::Point2F(right - fold, y), D2D1::Point2F(right - fold, y + fold), brush, strokeWidth);
    rt->DrawLine(D2D1::Point2F(right - fold, y + fold), D2D1::Point2F(right, y + fold), brush, strokeWidth);
}

void DialogRibbonOverlay::drawClockIcon(ID2D1RenderTarget* rt, ID2D1Brush* brush, float x, float y, float size, float strokeWidth) {
    if (!rt || !brush) return;
    const float cx = x + size * 0.5f;
    const float cy = y + size * 0.5f;
    const float r = size * 0.48f;

    rt->DrawEllipse(D2D1::Ellipse(D2D1::Point2F(cx, cy), r, r), brush, strokeWidth);
    rt->DrawLine(D2D1::Point2F(cx, cy), D2D1::Point2F(cx, cy - r * 0.58f), brush, strokeWidth);
    rt->DrawLine(D2D1::Point2F(cx, cy), D2D1::Point2F(cx + r * 0.45f, cy), brush, strokeWidth);
}

void DialogRibbonOverlay::drawBookmarkIcon(ID2D1RenderTarget* rt, ID2D1Brush* brush, float x, float y, float size, float strokeWidth) {
    if (!rt || !brush) return;
    const float w = size * 0.75f;
    const float h = size;
    const float left = x + (size - w) * 0.5f;
    const float right = left + w;
    const float bottom = y + h;
    const float notchY = bottom - h * 0.28f;
    const float midX = left + w * 0.5f;

    rt->DrawLine(D2D1::Point2F(left, y), D2D1::Point2F(right, y), brush, strokeWidth);
    rt->DrawLine(D2D1::Point2F(right, y), D2D1::Point2F(right, bottom), brush, strokeWidth);
    rt->DrawLine(D2D1::Point2F(right, bottom), D2D1::Point2F(midX, notchY), brush, strokeWidth);
    rt->DrawLine(D2D1::Point2F(midX, notchY), D2D1::Point2F(left, bottom), brush, strokeWidth);
    rt->DrawLine(D2D1::Point2F(left, bottom), D2D1::Point2F(left, y), brush, strokeWidth);
}

void DialogRibbonOverlay::drawChevron(ID2D1RenderTarget* rt, ID2D1Brush* brush, float centerX, float centerY, float size, float strokeWidth) {
    if (!rt || !brush) return;
    const float halfW = size * 0.5f;
    const float halfH = size * 0.28f;
    rt->DrawLine(D2D1::Point2F(centerX - halfW, centerY - halfH), D2D1::Point2F(centerX, centerY + halfH), brush, strokeWidth, nullptr);
    rt->DrawLine(D2D1::Point2F(centerX, centerY + halfH), D2D1::Point2F(centerX + halfW, centerY - halfH), brush, strokeWidth, nullptr);
}

void DialogRibbonOverlay::drawCloseIcon(ID2D1RenderTarget* rt, ID2D1Brush* brush, float centerX, float centerY, float size, float strokeWidth) {
    if (!rt || !brush) return;
    const float half = size * 0.5f;
    rt->DrawLine(D2D1::Point2F(centerX - half, centerY - half), D2D1::Point2F(centerX + half, centerY + half), brush, strokeWidth, nullptr);
    rt->DrawLine(D2D1::Point2F(centerX + half, centerY - half), D2D1::Point2F(centerX - half, centerY + half), brush, strokeWidth, nullptr);
}

void DialogRibbonOverlay::drawBadge(ID2D1RenderTarget* rt, ID2D1Brush* bgBrush, ID2D1Brush* textBrush, float x, float y, float w, float h, int count) {
    if (!rt || !bgBrush || !textBrush || !m_badgeFormat) return;
    D2D1_RECT_F badgeRect = D2D1::RectF(x, y, x + w, y + h);
    const float radius = h * 0.5f;
    rt->FillRoundedRectangle(D2D1::RoundedRect(badgeRect, radius, radius), bgBrush);

    std::wstring countStr = std::to_wstring(count);
    m_badgeFormat->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
    m_badgeFormat->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
    rt->DrawText(countStr.c_str(), static_cast<UINT32>(countStr.length()),
                 m_badgeFormat.Get(), badgeRect, textBrush);
}

// ============================================================
// 主胶囊按钮布局与渲染
// ============================================================
void DialogRibbonOverlay::updateButtonsLayout(float dpiScale, int maxAvailableWidth) {
    const float safeScale = std::clamp(dpiScale, 0.75f, 4.0f);
    m_buttons.clear();
    ensureTextFormats(safeScale);

    try {
        m_activeExplorerPath = ExplorerTracker::instance().getActiveExplorerPath();
    } catch (...) {
        m_activeExplorerPath.clear();
    }

    auto measureWithFormat = [&](const std::wstring& wText, IDWriteTextFormat* format) -> float {
        if (wText.empty()) return 0.0f;
        if (!format || !m_dwriteFactory) {
            return static_cast<float>(wText.length()) * 8.0f * safeScale;
        }
        ComPtr<IDWriteTextLayout> layout;
        const HRESULT hr = m_dwriteFactory->CreateTextLayout(
            wText.c_str(), static_cast<UINT32>(wText.length()),
            format, 2000.0f, 100.0f, layout.GetAddressOf());
        if (SUCCEEDED(hr) && layout) {
            layout->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
            DWRITE_TEXT_METRICS metrics{};
            layout->GetMetrics(&metrics);
            return std::ceil(metrics.widthIncludingTrailingWhitespace);
        }
        return static_cast<float>(wText.length()) * 8.0f * safeScale;
    };

    auto measureText = [&](const std::string& utf8Text, bool isBold) -> float {
        const std::wstring wText = tools3000::core::WinUtils::utf8ToWstring(utf8Text);
        return measureWithFormat(wText, isBold ? m_boldFormat.Get() : m_textFormat.Get());
    };

    m_height = static_cast<int>(std::round(25.0f * safeScale));
    const float paddingX = 4.5f * safeScale;
    const float btnH = 19.0f * safeScale;
    const float btnY = (static_cast<float>(m_height) - btnH) * 0.5f;
    const float syncDividerGap = 9.0f * safeScale;
    const float interButtonGap = 3.5f * safeScale;
    const float minSyncWidth = 68.0f * safeScale;
    const float searchW = 22.0f * safeScale;
    const float closeW = 20.0f * safeScale;

    bool hasSearch = PathMemoryManager::instance().isInlineSearchEnabled();

    bool hasSync = false;
    std::string syncLabel;
    float syncNaturalW = 0.0f;
    if (PathMemoryManager::instance().isQuickSwitchEnabled() && !m_activeExplorerPath.empty()) {
        std::string folderName = m_activeExplorerPath;
        const size_t lastSlash = folderName.find_last_of("\\/");
        if (lastSlash != std::string::npos && lastSlash + 1 < folderName.length()) {
            folderName = folderName.substr(lastSlash + 1);
        }
        if (folderName.empty()) folderName = m_activeExplorerPath;
        syncLabel = folderName;
        const float syncTextW = measureText(syncLabel, true) + 2.0f * safeScale;
        syncNaturalW = std::clamp(syncTextW + 33.0f * safeScale, minSyncWidth, 185.0f * safeScale);
        hasSync = true;
    }

    bool hasRecent = false;
    int recentCount = 0;
    const std::string recentLabel = tools3000::core::trUtf8("最近", "Recent");
    float recentTextW = 0.0f;
    float recentBadgeW = 0.0f;
    float recentW = 0.0f;
    try {
        auto recentList = PathMemoryManager::instance().getRecentPaths(5);
        recentCount = static_cast<int>(recentList.size());
        recentTextW = measureText(recentLabel, false) + 2.0f * safeScale;
        const float badgeDigitsW = measureWithFormat(std::to_wstring(recentCount), m_badgeFormat.Get());
        recentBadgeW = (std::max)(16.0f * safeScale, badgeDigitsW + 8.0f * safeScale);
        recentW = recentTextW + recentBadgeW + 41.0f * safeScale;
        hasRecent = true;
    } catch (...) {
        hasRecent = false;
    }

    const std::string favLabel = tools3000::core::trUtf8("工作区", "Workspaces");
    const float favTextW = measureText(favLabel, false) + 2.0f * safeScale;
    const float favW = favTextW + 38.0f * safeScale;
    bool hasFav = true;
    bool hasClose = true;

    float syncFinalW = syncNaturalW;
    auto calcTotalWidth = [&]() -> float {
        float w = paddingX;
        int primaryCount = 0;
        if (hasSearch) {
            w += searchW;
            if (hasSync || hasRecent || hasFav || hasClose) w += interButtonGap;
            ++primaryCount;
        }
        if (hasSync) {
            w += syncFinalW;
            if (hasRecent || hasFav || hasClose) w += syncDividerGap;
            ++primaryCount;
        }
        if (hasRecent) {
            w += recentW;
            if (hasFav || hasClose) w += interButtonGap;
            ++primaryCount;
        }
        if (hasFav) {
            w += favW;
            if (hasClose) w += interButtonGap;
            ++primaryCount;
        }
        if (primaryCount == 0) return 0.0f;
        if (hasClose) {
            w += closeW;
        }
        return w + paddingX;
    };

    if (maxAvailableWidth > 0) {
        const float maxW = static_cast<float>(maxAvailableWidth);
        float total = calcTotalWidth();
        if (total > maxW && hasSync) {
            const float overflow = total - maxW;
            const float shrinkable = syncFinalW - minSyncWidth;
            if (shrinkable >= overflow) {
                syncFinalW -= overflow;
            } else {
                hasSync = false;
            }
            total = calcTotalWidth();
        }
        if (total > maxW && hasRecent) {
            hasRecent = false;
            total = calcTotalWidth();
        }
        if (total > maxW && hasClose) {
            hasClose = false;
            total = calcTotalWidth();
        }
        if (total > maxW && hasFav) {
            hasFav = false;
            total = calcTotalWidth();
        }
        if (total > maxW && hasSearch) {
            hasSearch = false;
        }
    }

    float currentX = paddingX;
    if (hasSearch) {
        m_buttons.push_back({
            RibbonButtonRect::Type::Search,
            D2D1::RectF(currentX, btnY, currentX + searchW, btnY + btnH),
            "",
            tools3000::core::trUtf8("即搜即跳 (Ctrl+G)", "Quick Search & Jump (Ctrl+G)"),
            0, false, 0.0f, 0.0f
        });
        currentX += searchW + ((hasSync || hasRecent || hasFav || hasClose) ? interButtonGap : 0.0f);
    }

    if (hasSync) {
        m_buttons.push_back({
            RibbonButtonRect::Type::ExplorerSync,
            D2D1::RectF(currentX, btnY, currentX + syncFinalW, btnY + btnH),
            syncLabel,
            m_activeExplorerPath,
            0, false,
            (std::max)(0.0f, syncFinalW - 33.0f * safeScale),
            0.0f
        });
        currentX += syncFinalW + ((hasRecent || hasFav || hasClose) ? syncDividerGap : 0.0f);
    }

    if (hasRecent) {
        m_buttons.push_back({
            RibbonButtonRect::Type::Recent,
            D2D1::RectF(currentX, btnY, currentX + recentW, btnY + btnH),
            recentLabel, "", recentCount, false, recentTextW, recentBadgeW
        });
        currentX += recentW + ((hasFav || hasClose) ? interButtonGap : 0.0f);
    }

    if (hasFav) {
        m_buttons.push_back({
            RibbonButtonRect::Type::Favorites,
            D2D1::RectF(currentX, btnY, currentX + favW, btnY + btnH),
            favLabel, "", 0, false, favTextW, 0.0f
        });
        currentX += favW + (hasClose ? interButtonGap : 0.0f);
    }

    if (hasClose && !m_buttons.empty()) {
        m_buttons.push_back({
            RibbonButtonRect::Type::Close,
            D2D1::RectF(currentX, btnY, currentX + closeW, btnY + btnH),
            "", "", 0, false, 0.0f, 0.0f
        });
        currentX += closeW;
    }

    if (m_buttons.empty()) {
        m_width = 0;
    } else {
        m_width = static_cast<int>(std::ceil(currentX + paddingX));
        if (maxAvailableWidth > 0 && m_width > maxAvailableWidth) {
            m_width = maxAvailableWidth;
        }
    }
}

bool DialogRibbonOverlay::ensureDeviceBrushes(bool isDark, const tools3000::core::AccentColorRGB& accent) {
    if (!m_renderTarget) return false;
    const bool accentUnchanged =
        std::abs(m_brushesAccent.r - accent.r) < 0.002f &&
        std::abs(m_brushesAccent.g - accent.g) < 0.002f &&
        std::abs(m_brushesAccent.b - accent.b) < 0.002f;
    if (m_brushesValid && m_brushesDarkTheme == isDark && accentUnchanged &&
        m_bgBrush && m_textBrush && m_whiteBrush && m_syncBgBrush) {
        return true;
    }

    m_bgBrush.Reset();
    m_borderBrush.Reset();
    m_textBrush.Reset();
    m_textMutedBrush.Reset();
    m_highlightBrush.Reset();
    m_highlightBorderBrush.Reset();
    m_syncBgBrush.Reset();
    m_syncBorderBrush.Reset();
    m_chevronBrush.Reset();
    m_badgeBgBrush.Reset();
    m_badgeTextBrush.Reset();
    m_dividerBrush.Reset();
    m_whiteBrush.Reset();

    m_renderTarget->CreateSolidColorBrush(D2D1::ColorF(1.0f, 1.0f, 1.0f, 1.0f), m_whiteBrush.GetAddressOf());

    const float borderR = (std::min)(1.0f, accent.r + (1.0f - accent.r) * 0.30f);
    const float borderG = (std::min)(1.0f, accent.g + (1.0f - accent.g) * 0.30f);
    const float borderB = (std::min)(1.0f, accent.b + (1.0f - accent.b) * 0.30f);

    if (isDark) {
        m_renderTarget->CreateSolidColorBrush(D2D1::ColorF(0.12f, 0.13f, 0.17f, 0.94f), m_bgBrush.GetAddressOf());
        m_renderTarget->CreateSolidColorBrush(D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.14f), m_borderBrush.GetAddressOf());
        m_renderTarget->CreateSolidColorBrush(D2D1::ColorF(0.96f, 0.97f, 0.98f, 0.96f), m_textBrush.GetAddressOf());
        m_renderTarget->CreateSolidColorBrush(D2D1::ColorF(0.72f, 0.76f, 0.82f, 0.88f), m_textMutedBrush.GetAddressOf());
        m_renderTarget->CreateSolidColorBrush(D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.10f), m_highlightBrush.GetAddressOf());
        m_renderTarget->CreateSolidColorBrush(D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.16f), m_highlightBorderBrush.GetAddressOf());
        m_renderTarget->CreateSolidColorBrush(D2D1::ColorF(accent.r, accent.g, accent.b, 0.95f), m_syncBgBrush.GetAddressOf());
        m_renderTarget->CreateSolidColorBrush(D2D1::ColorF(borderR, borderG, borderB, 0.45f), m_syncBorderBrush.GetAddressOf());
        m_renderTarget->CreateSolidColorBrush(D2D1::ColorF(0.70f, 0.74f, 0.82f, 0.78f), m_chevronBrush.GetAddressOf());
        m_renderTarget->CreateSolidColorBrush(D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.12f), m_badgeBgBrush.GetAddressOf());
        m_renderTarget->CreateSolidColorBrush(D2D1::ColorF(0.95f, 0.95f, 0.98f, 0.95f), m_badgeTextBrush.GetAddressOf());
        m_renderTarget->CreateSolidColorBrush(D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.12f), m_dividerBrush.GetAddressOf());
    } else {
        m_renderTarget->CreateSolidColorBrush(D2D1::ColorF(0.965f, 0.975f, 0.985f, 0.96f), m_bgBrush.GetAddressOf());
        m_renderTarget->CreateSolidColorBrush(D2D1::ColorF(0.0f, 0.0f, 0.0f, 0.10f), m_borderBrush.GetAddressOf());
        m_renderTarget->CreateSolidColorBrush(D2D1::ColorF(0.10f, 0.12f, 0.16f, 0.95f), m_textBrush.GetAddressOf());
        m_renderTarget->CreateSolidColorBrush(D2D1::ColorF(0.32f, 0.36f, 0.44f, 0.88f), m_textMutedBrush.GetAddressOf());
        m_renderTarget->CreateSolidColorBrush(D2D1::ColorF(0.0f, 0.0f, 0.0f, 0.06f), m_highlightBrush.GetAddressOf());
        m_renderTarget->CreateSolidColorBrush(D2D1::ColorF(0.0f, 0.0f, 0.0f, 0.09f), m_highlightBorderBrush.GetAddressOf());
        m_renderTarget->CreateSolidColorBrush(D2D1::ColorF(accent.r * 0.94f, accent.g * 0.94f, accent.b * 0.94f, 0.95f), m_syncBgBrush.GetAddressOf());
        m_renderTarget->CreateSolidColorBrush(D2D1::ColorF(borderR, borderG, borderB, 0.50f), m_syncBorderBrush.GetAddressOf());
        m_renderTarget->CreateSolidColorBrush(D2D1::ColorF(0.40f, 0.44f, 0.52f, 0.78f), m_chevronBrush.GetAddressOf());
        m_renderTarget->CreateSolidColorBrush(D2D1::ColorF(0.0f, 0.0f, 0.0f, 0.06f), m_badgeBgBrush.GetAddressOf());
        m_renderTarget->CreateSolidColorBrush(D2D1::ColorF(0.18f, 0.22f, 0.28f, 0.92f), m_badgeTextBrush.GetAddressOf());
        m_renderTarget->CreateSolidColorBrush(D2D1::ColorF(0.0f, 0.0f, 0.0f, 0.09f), m_dividerBrush.GetAddressOf());
    }

    m_brushesDarkTheme = isDark;
    m_brushesAccent = accent;
    m_brushesValid = (m_bgBrush != nullptr && m_textBrush != nullptr && m_whiteBrush != nullptr);
    return m_brushesValid;
}

void DialogRibbonOverlay::render() {
    HWND hwnd = nullptr;
    int width = 0;
    int height = 0;
    int posX = 0;
    int posY = 0;
    float dpiScale = 1.0f;
    std::vector<RibbonButtonRect> buttons;
    bool searchOpen = false;

    {
        std::lock_guard lock(m_mutex);
        hwnd = m_hwnd;
        width = m_width;
        height = m_height;
        posX = m_posX;
        posY = m_posY;
        dpiScale = m_dpiScale;
        buttons = m_buttons;
        searchOpen = m_searchOpen;
    }

    if (!hwnd || !m_memDC || width <= 0 || height <= 0) return;

    // P1 退避护栏：D2D 连续失败时按退避窗口抑制无限重绘死循环
    if (m_d2dConsecutiveFailures > 0 && GetTickCount64() < m_d2dRetryAfterTick) {
        return;
    }

    if (m_memBitmap && (m_renderedWidth != width || m_renderedHeight != height)) {
        if (m_oldBitmap) {
            SelectObject(m_memDC, m_oldBitmap);
            m_oldBitmap = nullptr;
        }
        DeleteObject(m_memBitmap);
        m_memBitmap = nullptr;
    }

    if (!m_memBitmap) {
        HDC screenDC = GetDC(nullptr);
        BITMAPINFO bmi{};
        bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
        bmi.bmiHeader.biWidth = width;
        bmi.bmiHeader.biHeight = -height;
        bmi.bmiHeader.biPlanes = 1;
        bmi.bmiHeader.biBitCount = 32;
        bmi.bmiHeader.biCompression = BI_RGB;
        void* bits = nullptr;
        m_memBitmap = CreateDIBSection(screenDC, &bmi, DIB_RGB_COLORS, &bits, nullptr, 0);
        ReleaseDC(nullptr, screenDC);
        if (!m_memBitmap) return;
        m_oldBitmap = static_cast<HBITMAP>(SelectObject(m_memDC, m_memBitmap));
    }

    if (!m_renderTarget && m_d2dFactory) {
        D2D1_RENDER_TARGET_PROPERTIES rtProps = D2D1::RenderTargetProperties(
            D2D1_RENDER_TARGET_TYPE_DEFAULT,
            D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED),
            96.0f, 96.0f,
            D2D1_RENDER_TARGET_USAGE_GDI_COMPATIBLE);
        if (SUCCEEDED(m_d2dFactory->CreateDCRenderTarget(&rtProps, m_renderTarget.GetAddressOf())) && m_renderTarget) {
            m_renderTarget->SetDpi(96.0f, 96.0f);
            m_brushesValid = false;
        }
    }
    if (!m_renderTarget) return;

    ensureTextFormats(dpiScale);

    const bool isDark = tools3000::core::NativeThemePalette::isEffectiveDarkMode();
    const auto accent = tools3000::core::NativeThemePalette::getEffectiveAccentColorRGB();
    if (!ensureDeviceBrushes(isDark, accent)) {
        discardDeviceResources();
        return;
    }

    RECT rc = {0, 0, width, height};
    if (FAILED(m_renderTarget->BindDC(m_memDC, &rc))) {
        discardDeviceResources();
        return;
    }

    m_renderTarget->BeginDraw();
    m_renderTarget->Clear(D2D1::ColorF(0, 0, 0, 0));

    const float inset = 0.5f * dpiScale;
    const float capsuleRadius = (std::max)(0.0f, (static_cast<float>(height) * 0.5f) - inset);
    D2D1_ROUNDED_RECT mainRRect = D2D1::RoundedRect(
        D2D1::RectF(inset, inset, static_cast<float>(width) - inset, static_cast<float>(height) - inset),
        capsuleRadius, capsuleRadius);

    if (m_bgBrush) m_renderTarget->FillRoundedRectangle(&mainRRect, m_bgBrush.Get());
    if (m_borderBrush) m_renderTarget->DrawRoundedRectangle(&mainRRect, m_borderBrush.Get(), 1.0f * dpiScale);

    for (size_t i = 0; i < buttons.size(); ++i) {
        const auto& btn = buttons[i];
        const float btnRadius = (btn.rect.bottom - btn.rect.top) * 0.5f;
        D2D1_ROUNDED_RECT btnRRect = D2D1::RoundedRect(btn.rect, btnRadius, btnRadius);

        if (btn.type == RibbonButtonRect::Type::Search) {
            if (searchOpen && m_syncBgBrush) {
                m_syncBgBrush->SetOpacity(0.95f);
                m_renderTarget->FillRoundedRectangle(&btnRRect, m_syncBgBrush.Get());
            } else if (btn.hovered && m_highlightBrush) {
                m_renderTarget->FillRoundedRectangle(&btnRRect, m_highlightBrush.Get());
                if (m_highlightBorderBrush) {
                    m_renderTarget->DrawRoundedRectangle(&btnRRect, m_highlightBorderBrush.Get(), 1.0f * dpiScale);
                }
            }
            const float iconSize = 10.5f * dpiScale;
            const float ix = (btn.rect.left + btn.rect.right - iconSize) * 0.5f;
            const float iy = (btn.rect.top + btn.rect.bottom - iconSize) * 0.5f;
            ID2D1Brush* iconBrush = searchOpen
                ? m_whiteBrush.Get()
                : (btn.hovered ? m_textBrush.Get() : m_textMutedBrush.Get());
            drawSearchIcon(m_renderTarget.Get(), iconBrush, ix, iy, iconSize, 1.3f * dpiScale);
        } else if (btn.type == RibbonButtonRect::Type::ExplorerSync && m_syncBgBrush) {
            m_syncBgBrush->SetOpacity(btn.hovered ? 1.0f : 0.92f);
            m_renderTarget->FillRoundedRectangle(&btnRRect, m_syncBgBrush.Get());
            if (m_syncBorderBrush) {
                m_renderTarget->DrawRoundedRectangle(&btnRRect, m_syncBorderBrush.Get(), 1.0f * dpiScale);
            }
            const float iconSize = 12.0f * dpiScale;
            const float iconX = btn.rect.left + 8.0f * dpiScale;
            const float iconY = (btn.rect.top + btn.rect.bottom - iconSize) * 0.5f;
            drawFolderIcon(m_renderTarget.Get(), m_whiteBrush.Get(), iconX, iconY, iconSize, 1.2f * dpiScale);

            const std::wstring wText = tools3000::core::WinUtils::utf8ToWstring(btn.text);
            if (m_whiteBrush && m_boldFormat) {
                D2D1_RECT_F textRect = D2D1::RectF(
                    iconX + iconSize + 5.0f * dpiScale, btn.rect.top,
                    btn.rect.right - 8.0f * dpiScale, btn.rect.bottom);
                m_boldFormat->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
                m_boldFormat->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
                m_renderTarget->DrawText(
                    wText.c_str(), static_cast<UINT32>(wText.length()),
                    m_boldFormat.Get(), textRect, m_whiteBrush.Get());
            }

            if (m_dividerBrush && (i + 1 < buttons.size())) {
                const float divX = btn.rect.right + 5.0f * dpiScale;
                const float divY1 = (static_cast<float>(height) - 14.0f * dpiScale) * 0.5f;
                const float divY2 = divY1 + 14.0f * dpiScale;
                m_renderTarget->DrawLine(D2D1::Point2F(divX, divY1), D2D1::Point2F(divX, divY2),
                                         m_dividerBrush.Get(), 1.0f * dpiScale);
            }
        } else if (btn.type == RibbonButtonRect::Type::Recent) {
            if (btn.hovered && m_highlightBrush) {
                m_renderTarget->FillRoundedRectangle(&btnRRect, m_highlightBrush.Get());
                if (m_highlightBorderBrush) {
                    m_renderTarget->DrawRoundedRectangle(&btnRRect, m_highlightBorderBrush.Get(), 1.0f * dpiScale);
                }
            }
            auto activeTextBrush = btn.hovered ? m_textBrush.Get() : m_textMutedBrush.Get();
            auto activeChevronBrush = btn.hovered ? m_textBrush.Get() : m_chevronBrush.Get();

            const float iconSize = 11.5f * dpiScale;
            const float iconX = btn.rect.left + 7.0f * dpiScale;
            const float iconY = (btn.rect.top + btn.rect.bottom - iconSize) * 0.5f;
            drawClockIcon(m_renderTarget.Get(), activeTextBrush, iconX, iconY, iconSize, 1.15f * dpiScale);

            const std::wstring wText = tools3000::core::WinUtils::utf8ToWstring(btn.text);
            const float textLeft = iconX + iconSize + 4.5f * dpiScale;
            const float textW = btn.textWidth > 0.0f ? btn.textWidth : (36.0f * dpiScale);
            if (activeTextBrush && m_textFormat) {
                D2D1_RECT_F textRect = D2D1::RectF(textLeft, btn.rect.top, textLeft + textW, btn.rect.bottom);
                m_textFormat->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
                m_textFormat->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
                m_renderTarget->DrawText(
                    wText.c_str(), static_cast<UINT32>(wText.length()),
                    m_textFormat.Get(), textRect, activeTextBrush);
            }

            const float badgeW = btn.badgeWidth > 0.0f ? btn.badgeWidth : (16.0f * dpiScale);
            const float badgeH = 13.0f * dpiScale;
            const float badgeX = textLeft + textW + 3.0f * dpiScale;
            const float badgeY = (btn.rect.top + btn.rect.bottom - badgeH) * 0.5f;
            drawBadge(m_renderTarget.Get(), m_badgeBgBrush.Get(), m_badgeTextBrush.Get(),
                      badgeX, badgeY, badgeW, badgeH, btn.badgeCount);

            drawChevron(m_renderTarget.Get(), activeChevronBrush,
                        btn.rect.right - 7.5f * dpiScale,
                        (btn.rect.top + btn.rect.bottom) * 0.5f,
                        5.5f * dpiScale, 1.2f * dpiScale);
        } else if (btn.type == RibbonButtonRect::Type::Favorites) {
            if (btn.hovered && m_highlightBrush) {
                m_renderTarget->FillRoundedRectangle(&btnRRect, m_highlightBrush.Get());
                if (m_highlightBorderBrush) {
                    m_renderTarget->DrawRoundedRectangle(&btnRRect, m_highlightBorderBrush.Get(), 1.0f * dpiScale);
                }
            }
            auto activeTextBrush = btn.hovered ? m_textBrush.Get() : m_textMutedBrush.Get();
            auto activeChevronBrush = btn.hovered ? m_textBrush.Get() : m_chevronBrush.Get();

            const float iconSize = 11.0f * dpiScale;
            const float iconX = btn.rect.left + 7.0f * dpiScale;
            const float iconY = (btn.rect.top + btn.rect.bottom - iconSize) * 0.5f;
            drawBookmarkIcon(m_renderTarget.Get(), activeTextBrush, iconX, iconY, iconSize, 1.15f * dpiScale);

            const std::wstring wText = tools3000::core::WinUtils::utf8ToWstring(btn.text);
            const float textLeft = iconX + iconSize + 4.5f * dpiScale;
            const float textW = btn.textWidth > 0.0f ? btn.textWidth : (48.0f * dpiScale);
            if (activeTextBrush && m_textFormat) {
                D2D1_RECT_F textRect = D2D1::RectF(textLeft, btn.rect.top, textLeft + textW, btn.rect.bottom);
                m_textFormat->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
                m_textFormat->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
                m_renderTarget->DrawText(
                    wText.c_str(), static_cast<UINT32>(wText.length()),
                    m_textFormat.Get(), textRect, activeTextBrush);
            }

            drawChevron(m_renderTarget.Get(), activeChevronBrush,
                        btn.rect.right - 7.5f * dpiScale,
                        (btn.rect.top + btn.rect.bottom) * 0.5f,
                        5.5f * dpiScale, 1.2f * dpiScale);
        } else if (btn.type == RibbonButtonRect::Type::Close) {
            if (btn.hovered && m_highlightBrush) {
                m_renderTarget->FillRoundedRectangle(&btnRRect, m_highlightBrush.Get());
                if (m_highlightBorderBrush) {
                    m_renderTarget->DrawRoundedRectangle(&btnRRect, m_highlightBorderBrush.Get(), 1.0f * dpiScale);
                }
            }
            auto closeIconBrush = btn.hovered ? m_textBrush.Get() : m_chevronBrush.Get();
            drawCloseIcon(m_renderTarget.Get(), closeIconBrush,
                          (btn.rect.left + btn.rect.right) * 0.5f,
                          (btn.rect.top + btn.rect.bottom) * 0.5f,
                          7.0f * dpiScale, 1.25f * dpiScale);
        }
    }

    const HRESULT endHr = m_renderTarget->EndDraw();
    if (endHr == D2DERR_RECREATE_TARGET || FAILED(endHr)) {
        ++m_d2dConsecutiveFailures;
        static constexpr ULONGLONG kBackoffMs[] = {100, 300, 800};
        const ULONGLONG delay = kBackoffMs[(std::min)(m_d2dConsecutiveFailures - 1, 2)];
        m_d2dRetryAfterTick = GetTickCount64() + delay;

        LOG_WARN("DialogRibbonOverlay: EndDraw returned 0x{:X} (attempt {}/{}), resetting D2D target",
                 static_cast<unsigned>(endHr), m_d2dConsecutiveFailures, kMaxD2dConsecutiveFailures);
        discardDeviceResources();
        {
            std::lock_guard lock(m_mutex);
            m_renderedWidth = 0;
            m_renderedHeight = 0;
        }
        // 最多自动重试 3 次，杜绝驱动异常时的无限消息死循环
        if (m_d2dConsecutiveFailures <= kMaxD2dConsecutiveFailures && IsWindow(hwnd)) {
            PostMessageW(hwnd, WM_RIBBON_UPDATE_POS, 0, 0);
        }
        return;
    }

    m_d2dConsecutiveFailures = 0;
    m_d2dRetryAfterTick = 0;
    {
        std::lock_guard lock(m_mutex);
        m_renderedWidth = width;
        m_renderedHeight = height;
    }

    HDC screenDC = GetDC(nullptr);
    POINT ptSrc = {0, 0};
    POINT ptDst = {posX, posY};
    if (ptDst.x == 0 && ptDst.y == 0) {
        RECT winRect{};
        if (GetWindowRect(hwnd, &winRect)) {
            ptDst = {winRect.left, winRect.top};
        }
    }
    SIZE size = {width, height};
    BLENDFUNCTION blend = {AC_SRC_OVER, 0, 255, AC_SRC_ALPHA};
    UpdateLayeredWindow(hwnd, screenDC, &ptDst, &size, m_memDC, &ptSrc, 0, &blend, ULW_ALPHA);
    ReleaseDC(nullptr, screenDC);
}

bool DialogRibbonOverlay::hitTestButton(const RibbonButtonRect& btn,
                                        int x,
                                        int y,
                                        float dpiScale) const noexcept {
    const float safeScale = std::clamp(dpiScale, 0.75f, 4.0f);
    const float padX = (btn.type == RibbonButtonRect::Type::Close || btn.type == RibbonButtonRect::Type::Search)
        ? (2.0f * safeScale)
        : 0.0f;
    const float padY = 2.0f * safeScale;
    const float fx = static_cast<float>(x);
    const float fy = static_cast<float>(y);
    return fx >= (btn.rect.left - padX) && fx <= (btn.rect.right + padX) &&
           fy >= (btn.rect.top - padY) && fy <= (btn.rect.bottom + padY);
}

void DialogRibbonOverlay::updateTooltipText(const std::string& utf8Tip) {
    HWND hwnd = nullptr;
    HWND tooltipHwnd = nullptr;
    {
        std::lock_guard lock(m_mutex);
        if (m_currentTooltipText == utf8Tip) return;
        m_currentTooltipText = utf8Tip;
        hwnd = m_hwnd;
        tooltipHwnd = m_tooltipHwnd;
    }
    if (!tooltipHwnd || !IsWindow(tooltipHwnd) || !hwnd || !IsWindow(hwnd)) return;

    std::wstring wTip = tools3000::core::WinUtils::utf8ToWstring(utf8Tip);
    TTTOOLINFOW ti{};
    ti.cbSize = sizeof(TTTOOLINFOW);
    ti.hwnd = hwnd;
    ti.uId = 1;
    ti.hinst = resolveOverlayModuleInstance();
    ti.lpszText = wTip.empty() ? const_cast<LPWSTR>(L"") : wTip.data();
    SendMessageW(tooltipHwnd, TTM_UPDATETIPTEXTW, 0, reinterpret_cast<LPARAM>(&ti));
    if (wTip.empty()) {
        SendMessageW(tooltipHwnd, TTM_POP, 0, 0);
    }
}

void DialogRibbonOverlay::onMouseMove(int x, int y) {
    HWND hwnd = getHwnd();
    if (!m_trackingMouse && hwnd && IsWindow(hwnd)) {
        TRACKMOUSEEVENT tme = {sizeof(TRACKMOUSEEVENT), TME_LEAVE, hwnd, 0};
        TrackMouseEvent(&tme);
        m_trackingMouse = true;
    }

    bool needRepaint = false;
    std::string hoveredTip;
    {
        std::lock_guard lock(m_mutex);
        for (auto& btn : m_buttons) {
            const bool inside = hitTestButton(btn, x, y, m_dpiScale);
            if (inside &&
                (btn.type == RibbonButtonRect::Type::ExplorerSync || btn.type == RibbonButtonRect::Type::Search) &&
                !btn.extraData.empty()) {
                hoveredTip = btn.extraData;
            }
            if (btn.hovered != inside) {
                btn.hovered = inside;
                needRepaint = true;
            }
        }
    }

    updateTooltipText(hoveredTip);
    if (needRepaint) render();
}

void DialogRibbonOverlay::onMouseLeave() {
    m_trackingMouse = false;
    bool needRepaint = false;
    {
        std::lock_guard lock(m_mutex);
        for (auto& btn : m_buttons) {
            if (btn.hovered) {
                btn.hovered = false;
                needRepaint = true;
            }
        }
    }
    updateTooltipText("");
    if (needRepaint) render();
}

void DialogRibbonOverlay::onLButtonDown(int x, int y) {
    RibbonButtonRect clickedBtn{};
    bool hit = false;
    HWND hwnd = nullptr;
    HWND targetDialog = nullptr;
    int width = 0;
    int height = 0;
    float dpiScale = 1.0f;

    {
        std::lock_guard lock(m_mutex);
        hwnd = m_hwnd;
        targetDialog = m_targetDialog;
        width = m_width;
        height = m_height;
        dpiScale = m_dpiScale;
        for (const auto& btn : m_buttons) {
            if (hitTestButton(btn, x, y, dpiScale)) {
                clickedBtn = btn;
                hit = true;
                break;
            }
        }
        if (hit && clickedBtn.type == RibbonButtonRect::Type::Close && targetDialog) {
            m_dismissedDialog = targetDialog;
        }
    }

    if (!hit) return;

    if (clickedBtn.type == RibbonButtonRect::Type::Close) {
        doHide();
        return;
    }

    if (clickedBtn.type == RibbonButtonRect::Type::Search) {
        if (m_searchOpen) {
            closeInlineSearchPopup(true);
        } else {
            openInlineSearchPopup();
        }
        return;
    }

    // 点击其他按钮前收起搜索框
    if (m_searchOpen) {
        closeInlineSearchPopup(false);
    }

    const std::string posConfig = PathMemoryManager::instance().getRibbonPosition();
    const bool rightAlign = (!isCenterDockPosition(posConfig)) ||
                            (clickedBtn.rect.right > static_cast<float>(width) * 0.55f);
    const int localAnchorX = rightAlign
        ? static_cast<int>(std::round(clickedBtn.rect.right))
        : static_cast<int>(std::round(clickedBtn.rect.left));
    const int localAnchorY = height + static_cast<int>(std::round(4.0f * dpiScale));
    POINT screenPt = {localAnchorX, localAnchorY};
    if (hwnd && IsWindow(hwnd)) {
        ClientToScreen(hwnd, &screenPt);
    }

    if (clickedBtn.type == RibbonButtonRect::Type::ExplorerSync) {
        if (!clickedBtn.extraData.empty() && targetDialog && IsWindow(targetDialog)) {
            const std::string navPath = clickedBtn.extraData;
            DialogNavigator::instance().navigateToFolderAsync(
                targetDialog,
                navPath,
                [this, targetDialog, navPath](bool ok) {
                    if (ok) {
                        setCachedDialogFolder(targetDialog, navPath);
                    }
                    updatePosition();
                });
        }
    } else if (clickedBtn.type == RibbonButtonRect::Type::Recent) {
        showRecentMenu(screenPt.x, screenPt.y, rightAlign);
    } else if (clickedBtn.type == RibbonButtonRect::Type::Favorites) {
        showFavoritesMenu(screenPt.x, screenPt.y, rightAlign);
    }
}

void DialogRibbonOverlay::showRecentMenu(int screenX, int screenY, bool rightAlign) {
    const std::vector<std::string> localRecentList = PathMemoryManager::instance().getRecentPaths(10);
    if (localRecentList.empty()) return;

    HWND localHwnd = nullptr;
    HWND localTargetDialog = nullptr;
    DWORD localTargetPid = 0;
    {
        std::lock_guard lock(m_mutex);
        localHwnd = m_hwnd;
        localTargetDialog = m_targetDialog;
        localTargetPid = m_targetProcessId;
        m_menuOpen = true;
    }
    if (!localHwnd || !IsWindow(localHwnd)) {
        std::lock_guard lock(m_mutex);
        m_menuOpen = false;
        return;
    }

    tools3000::core::WinUtils::applyNativeMenuTheme();
    HMENU hMenu = CreatePopupMenu();
    for (size_t i = 0; i < localRecentList.size(); ++i) {
        std::wstring itemText = tools3000::core::WinUtils::utf8ToWstring(localRecentList[i]);
        AppendMenuW(hMenu, MF_STRING, static_cast<UINT_PTR>(1000 + i), itemText.c_str());
    }

    SetForegroundWindow(localHwnd);
    const UINT alignFlags = (rightAlign ? TPM_RIGHTALIGN : TPM_LEFTALIGN) |
                            TPM_TOPALIGN | TPM_RETURNCMD | TPM_NONOTIFY;
    const int cmd = TrackPopupMenu(hMenu, alignFlags, screenX, screenY, 0, localHwnd, nullptr);
    DestroyMenu(hMenu);

    {
        std::lock_guard lock(m_mutex);
        m_menuOpen = false;
    }

    // P1 修复：菜单关闭后立即归还前景焦点给目标文件对话框
    if (localTargetDialog && IsWindow(localTargetDialog)) {
        SetForegroundWindow(localTargetDialog);
    }

    if (cmd >= 1000 && static_cast<size_t>(cmd - 1000) < localRecentList.size()) {
        const std::string chosenPath = localRecentList[static_cast<size_t>(cmd - 1000)];
        DWORD currentPid = 0;
        if (localTargetDialog && IsWindow(localTargetDialog)) {
            GetWindowThreadProcessId(localTargetDialog, &currentPid);
        }
        if (localTargetDialog && currentPid != 0 && currentPid == localTargetPid) {
            DialogNavigator::instance().navigateToFolderAsync(
                localTargetDialog,
                chosenPath,
                [this, localTargetDialog, chosenPath](bool ok) {
                    if (ok) {
                        setCachedDialogFolder(localTargetDialog, chosenPath);
                    }
                    updatePosition();
                });
        }
    } else {
        updatePosition();
    }
}

void DialogRibbonOverlay::showFavoritesMenu(int screenX, int screenY, bool rightAlign) {
    const std::vector<std::string> localFavList = PathMemoryManager::instance().getFavorites();
    HWND localHwnd = nullptr;
    HWND localTargetDialog = nullptr;
    DWORD localTargetPid = 0;
    std::string localTargetProcess;
    std::string localCurFolder;
    {
        std::lock_guard lock(m_mutex);
        localHwnd = m_hwnd;
        localTargetDialog = m_targetDialog;
        localTargetPid = m_targetProcessId;
        localTargetProcess = m_targetProcess;
        localCurFolder = m_cachedDialogFolder;
        m_menuOpen = true;
    }
    if (!localHwnd || !IsWindow(localHwnd)) {
        std::lock_guard lock(m_mutex);
        m_menuOpen = false;
        return;
    }

    tools3000::core::WinUtils::applyNativeMenuTheme();
    HMENU hMenu = CreatePopupMenu();

    const bool canOperateOnDialog = (localTargetDialog && IsWindow(localTargetDialog));
    if (canOperateOnDialog) {
        const std::wstring folderSuffix = localCurFolder.empty()
            ? std::wstring{}
            : (L" (" + tools3000::core::WinUtils::utf8ToWstring(localCurFolder) + L")");
        const std::wstring addFavText = tools3000::core::trFmtW(L"[+] 将当前目录加入常用工作区{0}", L"[+] Add Current Folder to Workspaces{0}", {folderSuffix});
        AppendMenuW(hMenu, MF_STRING, 2901, addFavText.c_str());

        if (!localTargetProcess.empty()) {
            const std::wstring wProc = tools3000::core::WinUtils::utf8ToWstring(localTargetProcess);
            const std::wstring lockText = tools3000::core::trFmtW(L"[*] 为当前软件 ({0}) 固定此目录", L"[*] Pin Folder for Current App ({0})", {wProc});
            AppendMenuW(hMenu, MF_STRING, 2902, lockText.c_str());
        }
        if (!localFavList.empty()) {
            AppendMenuW(hMenu, MF_SEPARATOR, 0, nullptr);
        }
    }

    for (size_t i = 0; i < localFavList.size(); ++i) {
        std::wstring itemText = tools3000::core::WinUtils::utf8ToWstring(localFavList[i]);
        AppendMenuW(hMenu, MF_STRING, static_cast<UINT_PTR>(2000 + i), itemText.c_str());
    }

    SetForegroundWindow(localHwnd);
    const UINT alignFlags = (rightAlign ? TPM_RIGHTALIGN : TPM_LEFTALIGN) |
                            TPM_TOPALIGN | TPM_RETURNCMD | TPM_NONOTIFY;
    const int cmd = TrackPopupMenu(hMenu, alignFlags, screenX, screenY, 0, localHwnd, nullptr);
    DestroyMenu(hMenu);

    {
        std::lock_guard lock(m_mutex);
        m_menuOpen = false;
    }

    // P1 修复：菜单关闭后立即归还前景焦点给目标文件对话框
    if (localTargetDialog && IsWindow(localTargetDialog)) {
        SetForegroundWindow(localTargetDialog);
    }

    auto resolveFolderAndRun = [this, localTargetDialog, localTargetPid, localCurFolder](
                                   std::function<void(const std::string&)> action) {
        if (!localCurFolder.empty()) {
            action(localCurFolder);
            return;
        }
        DialogNavigator::instance().postAsyncTask(
            [this, localTargetDialog, localTargetPid, cb = std::move(action)]() {
                DWORD pid = 0;
                if (!localTargetDialog || !IsWindow(localTargetDialog)) return;
                GetWindowThreadProcessId(localTargetDialog, &pid);
                if (pid == 0 || pid != localTargetPid) return;
                const std::string resolved = DialogNavigator::getCurrentDialogFolder(localTargetDialog);
                if (!resolved.empty()) {
                    setCachedDialogFolder(localTargetDialog, resolved);
                    cb(resolved);
                }
            });
    };

    if (cmd == 2901) {
        resolveFolderAndRun([](const std::string& folder) {
            PathMemoryManager::instance().addFavorite(folder);
        });
        updatePosition();
    } else if (cmd == 2902 && !localTargetProcess.empty()) {
        resolveFolderAndRun([localTargetProcess](const std::string& folder) {
            PathMemoryManager::instance().setAppFixedWorkspace(localTargetProcess, folder, true);
        });
        updatePosition();
    } else if (cmd >= 2000 && static_cast<size_t>(cmd - 2000) < localFavList.size()) {
        const std::string chosenPath = localFavList[static_cast<size_t>(cmd - 2000)];
        DWORD currentPid = 0;
        if (localTargetDialog && IsWindow(localTargetDialog)) {
            GetWindowThreadProcessId(localTargetDialog, &currentPid);
        }
        if (localTargetDialog && currentPid != 0 && currentPid == localTargetPid) {
            DialogNavigator::instance().navigateToFolderAsync(
                localTargetDialog,
                chosenPath,
                [this, localTargetDialog, chosenPath](bool ok) {
                    if (ok) {
                        setCachedDialogFolder(localTargetDialog, chosenPath);
                    }
                    updatePosition();
                });
        }
    } else {
        updatePosition();
    }
}

// ============================================================
// Listary 风格纯原生内嵌极速搜索即跳（0 WebView2 / 严守 DEMAND_START）
// ============================================================
bool DialogRibbonOverlay::ensureSearchPopupCreated() {
    if (m_searchPopupHwnd && IsWindow(m_searchPopupHwnd) &&
        m_searchEditHwnd && IsWindow(m_searchEditHwnd)) {
        return true;
    }

    HINSTANCE hInst = resolveOverlayModuleInstance();
    HWND popup = CreateWindowExW(
        WS_EX_TOOLWINDOW,
        SEARCH_POPUP_WINDOW_CLASS,
        L"Tools3000 Dialog Quick Search",
        WS_POPUP | WS_CLIPCHILDREN,
        0, 0, m_searchPopupWidth, m_searchPopupHeight,
        nullptr, nullptr, hInst, this);
    if (!popup) {
        LOG_WARN("DialogRibbonOverlay: failed to create inline search popup window");
        return false;
    }

    HWND edit = CreateWindowExW(
        0,
        L"EDIT",
        L"",
        WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL | ES_LEFT,
        0, 0, 200, 24,
        popup,
        reinterpret_cast<HMENU>(static_cast<UINT_PTR>(kSearchEditControlId)),
        hInst,
        nullptr);
    if (!edit) {
        DestroyWindow(popup);
        return false;
    }

    SetWindowSubclass(edit, SearchEditSubclassProc, kSearchEditSubclassId, reinterpret_cast<DWORD_PTR>(this));

    {
        std::lock_guard lock(m_mutex);
        m_searchPopupHwnd = popup;
        m_searchEditHwnd = edit;
    }
    return true;
}

void DialogRibbonOverlay::updateSearchEditVisuals(bool isDark, float dpiScale) {
    if (!m_searchEditHwnd || !IsWindow(m_searchEditHwnd)) return;

    const COLORREF bg = isDark ? RGB(26, 29, 38) : RGB(248, 250, 252);
    const COLORREF fg = isDark ? RGB(244, 246, 250) : RGB(20, 24, 32);

    if (!m_searchEditBgBrush || m_searchEditBgColor != bg) {
        if (m_searchEditBgBrush) DeleteObject(m_searchEditBgBrush);
        m_searchEditBgBrush = CreateSolidBrush(bg);
        m_searchEditBgColor = bg;
    }
    m_searchEditTextColor = fg;
    m_searchEditDarkTheme = isDark;

    if (!m_searchEditFont || std::abs(m_searchEditFontDpi - dpiScale) > 0.001f) {
        if (m_searchEditFont) DeleteObject(m_searchEditFont);
        const wchar_t* face = tools3000::core::resolveInstalledUiFontFamily(m_dwriteFactory.Get());
        const int heightPx = -static_cast<int>(std::lround(13.5f * dpiScale));
        m_searchEditFont = CreateFontW(
            heightPx, 0, 0, 0, FW_MEDIUM, FALSE, FALSE, FALSE,
            DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
            CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_SWISS, face);
        m_searchEditFontDpi = dpiScale;
        if (m_searchEditFont) {
            SendMessageW(m_searchEditHwnd, WM_SETFONT, reinterpret_cast<WPARAM>(m_searchEditFont), TRUE);
        }
    }

    const std::wstring cue = m_searchOnlyFolders
        ? tools3000::core::trW(L"搜索文件夹，回车秒跳...", L"Search folders, Enter to jump...")
        : tools3000::core::trW(L"搜索文件或文件夹，回车秒跳...", L"Search files or folders, Enter to jump...");
    SendMessageW(m_searchEditHwnd, EM_SETCUEBANNER, TRUE, reinterpret_cast<LPARAM>(cue.c_str()));
}

std::vector<InlineSearchEntry> DialogRibbonOverlay::collectLocalCandidates(
    const std::string& queryUtf8, bool /*onlyFolders*/) const {
    std::vector<InlineSearchEntry> out;
    std::unordered_set<std::string> seen;
    const auto tokens = splitFoldedTokens(queryUtf8);

    auto tryPush = [&](const std::string& rawPath, InlineSearchSource src) {
        if (rawPath.empty()) return;
        const std::string key = normalizeDedupKey(rawPath);
        if (key.empty() || !seen.insert(key).second) return;
        InlineSearchEntry e = makeLocalEntry(rawPath, src);
        if (!tokens.empty()) {
            const std::string folded = toLowerAsciiAndWideFold(e.name + " " + e.path);
            if (!matchesMultiTokenFolded(folded, tokens)) return;
        }
        out.push_back(std::move(e));
    };

    // 1. 已打开的 Explorer 标签页
    try {
        for (const auto& p : ExplorerTracker::instance().getAllOpenExplorerPaths()) {
            tryPush(p, InlineSearchSource::ExplorerTab);
        }
    } catch (...) {
    }

    // 2. 收藏工作区
    for (const auto& p : PathMemoryManager::instance().getFavorites()) {
        tryPush(p, InlineSearchSource::Favorite);
    }

    // 3. 当前应用记忆目录
    std::string proc;
    {
        std::lock_guard lock(m_mutex);
        proc = m_targetProcess;
    }
    if (!proc.empty()) {
        const std::string appMemPath = PathMemoryManager::instance().getEffectiveAppPath(proc);
        if (!appMemPath.empty()) {
            tryPush(appMemPath, InlineSearchSource::AppMemory);
        }
    }

    // 4. 全局最近使用目录 (MRU)
    for (const auto& p : PathMemoryManager::instance().getRecentPaths(10)) {
        tryPush(p, InlineSearchSource::Recent);
    }

    return out;
}

void DialogRibbonOverlay::rebuildLocalSearchResults(const std::string& queryUtf8) {
    m_searchResults = collectLocalCandidates(queryUtf8, m_searchOnlyFolders);
    m_searchSelectedIndex = 0;
    m_searchHoveredIndex = -1;
    m_searchScrollTop = 0;
    m_searchServiceStarting = false;
    m_searchServiceUnavailable = false;
    layoutSearchPopup();
    renderSearchPopup();
}

void DialogRibbonOverlay::openInlineSearchPopup() {
    HWND targetDialog = nullptr;
    DialogMode targetMode = DialogMode::Unknown;
    float dpiScale = 1.0f;
    {
        std::lock_guard lock(m_mutex);
        targetDialog = m_targetDialog;
        targetMode = m_targetDialogMode;
        dpiScale = m_dpiScale;
    }
    if (!targetDialog || !IsWindow(targetDialog)) return;
    if (!ensureSearchPopupCreated()) return;

    // 展开默认范围：SaveFile / PickFolder 默认仅文件夹，OpenFile / Unknown 默认全部
    m_searchOnlyFolders = (targetMode == DialogMode::SaveFile || targetMode == DialogMode::PickFolder);
    m_searchCurrentQuery.clear();
    m_searchOpen = true;
    m_searchCancelFlag.store(true, std::memory_order_release);
    m_searchQuerySeq.fetch_add(1, std::memory_order_acq_rel);

    const bool isDark = tools3000::core::NativeThemePalette::isEffectiveDarkMode();
    tools3000::core::WinUtils::applyImmersiveDarkMode(m_searchPopupHwnd, isDark);
    updateSearchEditVisuals(isDark, dpiScale);

    m_searchSuppressNextChange = true;
    SetWindowTextW(m_searchEditHwnd, L"");
    m_searchSuppressNextChange = false;

    // 0ms 零延迟本地首屏（严格 DEMAND_START：空查询绝不触碰 SearchPipeClient！）
    rebuildLocalSearchResults("");

    ShowWindow(m_searchPopupHwnd, SW_SHOW);
    SetForegroundWindow(m_searchPopupHwnd);
    SetFocus(m_searchEditHwnd);
    render();
    renderSearchPopup();
}

void DialogRibbonOverlay::closeInlineSearchPopup(bool restoreTargetFocus) {
    if (!m_searchOpen && (!m_searchPopupHwnd || !IsWindowVisible(m_searchPopupHwnd))) {
        return;
    }

    m_searchOpen = false;
    m_searchCancelFlag.store(true, std::memory_order_release);
    m_searchQuerySeq.fetch_add(1, std::memory_order_acq_rel);
    if (m_searchPopupHwnd && IsWindow(m_searchPopupHwnd)) {
        KillTimer(m_searchPopupHwnd, kSearchDebounceTimerId);
        ShowWindow(m_searchPopupHwnd, SW_HIDE);
    }

    HWND targetDialog = getTargetDialog();
    if (restoreTargetFocus && targetDialog && IsWindow(targetDialog)) {
        SetForegroundWindow(targetDialog);
    }
    render();
}

void DialogRibbonOverlay::layoutSearchPopup() {
    if (!m_searchPopupHwnd || !IsWindow(m_searchPopupHwnd)) return;

    HWND targetDialog = nullptr;
    float dpiScale = 1.0f;
    int ribbonX = 0;
    int ribbonY = 0;
    int ribbonW = 0;
    int ribbonH = 0;
    {
        std::lock_guard lock(m_mutex);
        targetDialog = m_targetDialog;
        dpiScale = m_dpiScale;
        ribbonX = m_posX;
        ribbonY = m_posY;
        ribbonW = m_width;
        ribbonH = m_height;
    }
    if (!targetDialog || !IsWindow(targetDialog)) return;

    RECT frameRect{};
    if (!getTargetVisibleFrameBounds(targetDialog, frameRect)) return;

    const HMONITOR monitor = MonitorFromWindow(targetDialog, MONITOR_DEFAULTTONEAREST);
    const RECT work = tools3000::core::dpi::workArea(monitor);

    const int dlgW = static_cast<int>(frameRect.right - frameRect.left);
    const int minW = static_cast<int>(std::lround(420.0f * dpiScale));
    const int maxW = static_cast<int>(std::lround(560.0f * dpiScale));
    const int desiredW = std::clamp(static_cast<int>(dlgW * 0.72f), minW, maxW);

    const int headerH = static_cast<int>(std::lround(42.0f * dpiScale));
    const int rowH = static_cast<int>(std::lround(44.0f * dpiScale));
    const int footerH = static_cast<int>(std::lround(26.0f * dpiScale));
    const int visibleRows = m_searchResults.empty()
        ? 1
        : (std::min)(static_cast<int>(m_searchResults.size()), kMaxVisibleSearchRows);
    const int desiredH = headerH + visibleRows * rowH + footerH + static_cast<int>(std::lround(8.0f * dpiScale));

    const int gap = static_cast<int>(std::lround(6.0f * dpiScale));
    int popX = ribbonX + ribbonW - desiredW;
    if (popX < frameRect.left + static_cast<int>(12.0f * dpiScale)) {
        popX = ribbonX;
    }
    int popY = ribbonY + ribbonH + gap;
    if (work.bottom > work.top && popY + desiredH > work.bottom) {
        popY = ribbonY - desiredH - gap;
    }

    const RECT clamped = tools3000::core::dpi::clampWindowToWorkArea(
        RECT{popX, popY, popX + desiredW, popY + desiredH}, work);
    m_searchPopupWidth = static_cast<int>(clamped.right - clamped.left);
    m_searchPopupHeight = static_cast<int>(clamped.bottom - clamped.top);

    applyRelativeZOrder(m_searchPopupHwnd, targetDialog,
                        clamped.left, clamped.top, m_searchPopupWidth, m_searchPopupHeight, 0);
    tools3000::core::WinUtils::applyUniversalRoundedCorners(
        m_searchPopupHwnd, m_searchPopupWidth, m_searchPopupHeight,
        static_cast<int>(std::lround(10.0f * dpiScale)));

    if (m_searchEditHwnd && IsWindow(m_searchEditHwnd)) {
        const int badgeW = static_cast<int>(std::lround(74.0f * dpiScale));
        const int editLeft = static_cast<int>(std::lround(36.0f * dpiScale));
        const int editRight = m_searchPopupWidth - badgeW - static_cast<int>(std::lround(16.0f * dpiScale));
        const int editH = static_cast<int>(std::lround(22.0f * dpiScale));
        const int editTop = (headerH - editH) / 2 + static_cast<int>(std::lround(2.0f * dpiScale));
        MoveWindow(m_searchEditHwnd, editLeft, editTop,
                   (std::max)(80, editRight - editLeft), editH, TRUE);
    }
}

void DialogRibbonOverlay::onSearchQueryTextChanged() {
    if (m_searchSuppressNextChange || !m_searchEditHwnd || !IsWindow(m_searchEditHwnd)) return;

    const int len = GetWindowTextLengthW(m_searchEditHwnd);
    std::wstring wQuery(static_cast<size_t>((std::max)(0, len)) + 1, L'\0');
    const int copied = GetWindowTextW(m_searchEditHwnd, wQuery.data(), static_cast<int>(wQuery.size()));
    wQuery.resize(static_cast<size_t>((std::max)(0, copied)));
    m_searchCurrentQuery = tools3000::core::WinUtils::wstringToUtf8(wQuery);

    // 取消任何在途旧防抖与旧查询
    if (m_searchPopupHwnd && IsWindow(m_searchPopupHwnd)) {
        KillTimer(m_searchPopupHwnd, kSearchDebounceTimerId);
    }
    m_searchCancelFlag.store(true, std::memory_order_release);
    m_searchQuerySeq.fetch_add(1, std::memory_order_acq_rel);

    // 1. 0ms 本地瞬时过滤，零打字空白闪烁
    rebuildLocalSearchResults(m_searchCurrentQuery);

    // 2. 严格 DEMAND_START：仅在存在非空查询字符时启动 40ms 防抖并调用全盘搜索服务！
    if (!tools3000::core::ipc::SearchPipeClient::shouldAllowAutoStart(
            m_searchCurrentQuery, /*explicitUserTyped=*/true, /*autoStartRequested=*/true)) {
        return;
    }

    if (m_searchPopupHwnd && IsWindow(m_searchPopupHwnd)) {
        SetTimer(m_searchPopupHwnd, kSearchDebounceTimerId, kSearchDebounceMs, nullptr);
    }
}

void DialogRibbonOverlay::startSearchWorker() {
    std::lock_guard lock(m_searchWorkerMutex);
    if (m_searchWorkerRunning.load(std::memory_order_acquire)) return;
    if (m_searchWorker.joinable()) {
        m_searchWorker.join();
    }
    m_searchWorkerRunning.store(true, std::memory_order_release);
    m_searchWorker = std::thread([this]() noexcept {
        searchWorkerLoop();
    });
}

void DialogRibbonOverlay::stopSearchWorker() {
    m_searchCancelFlag.store(true, std::memory_order_release);
    m_searchWorkerRunning.store(false, std::memory_order_release);
    {
        std::lock_guard lock(m_searchWorkerMutex);
        m_pendingSearchJob.reset();
    }
    m_searchWorkerCv.notify_all();
    if (m_searchWorker.joinable() && m_searchWorker.get_id() != std::this_thread::get_id()) {
        m_searchWorker.join();
    }
}

void DialogRibbonOverlay::dispatchDebouncedDiskSearch() {
    if (!m_searchOpen) return;
    if (!tools3000::core::ipc::SearchPipeClient::shouldAllowAutoStart(
            m_searchCurrentQuery, true, true)) {
        return;
    }

    startSearchWorker();
    const uint64_t seq = m_searchQuerySeq.fetch_add(1, std::memory_order_acq_rel) + 1;
    m_searchCancelFlag.store(false, std::memory_order_release);

    std::string curFolder;
    std::string targetProc;
    {
        std::lock_guard lock(m_mutex);
        curFolder = m_cachedDialogFolder;
        targetProc = m_targetProcess;
    }

    {
        std::lock_guard lock(m_searchWorkerMutex);
        m_pendingSearchJob = SearchWorkerJob{
            seq, m_searchCurrentQuery, m_searchOnlyFolders, std::move(curFolder), std::move(targetProc)
        };
    }
    m_searchWorkerCv.notify_one();
}

void DialogRibbonOverlay::searchWorkerLoop() {
    while (m_searchWorkerRunning.load(std::memory_order_acquire)) {
        std::optional<SearchWorkerJob> job;
        {
            std::unique_lock lock(m_searchWorkerMutex);
            m_searchWorkerCv.wait(lock, [this]() {
                return !m_searchWorkerRunning.load(std::memory_order_acquire) || m_pendingSearchJob.has_value();
            });
            if (!m_searchWorkerRunning.load(std::memory_order_acquire)) break;
            job = std::move(m_pendingSearchJob);
            m_pendingSearchJob.reset();
        }
        if (!job) continue;
        if (m_searchQuerySeq.load(std::memory_order_acquire) != job->seq) continue;

        // 注入当前对话框上下文高优目录（当前目录、Explorer、应用记忆、收藏）
        std::vector<tools3000::core::ipc::DialogPriorityEntry> priorities;
        if (!job->currentFolder.empty()) {
            priorities.push_back({job->currentFolder, 6500.0});
        }
        const std::string activeExp = ExplorerTracker::instance().getActiveExplorerPath();
        if (!activeExp.empty()) {
            priorities.push_back({activeExp, 5800.0});
        }
        if (!job->targetProcess.empty()) {
            const std::string mem = PathMemoryManager::instance().getEffectiveAppPath(job->targetProcess);
            if (!mem.empty()) priorities.push_back({mem, 5500.0});
        }
        for (const auto& fav : PathMemoryManager::instance().getFavorites()) {
            if (!fav.empty()) priorities.push_back({fav, 4800.0});
        }

        const auto resp = tools3000::core::ipc::SearchPipeClient::instance().queryFiles(
            job->query,
            job->seq,
            28,
            job->onlyFolders,
            priorities,
            /*autoStart=*/true,
            /*timeoutMs=*/1200,
            &m_searchCancelFlag);

        if (!m_searchWorkerRunning.load(std::memory_order_acquire) ||
            m_searchQuerySeq.load(std::memory_order_acquire) != job->seq) {
            continue;
        }

        PendingSearchResult pending;
        pending.seq = job->seq;
        pending.elapsedMs = resp.elapsedMs;

        // 将匹配的本地候选置顶，再去重追加全盘结果
        pending.entries = collectLocalCandidates(job->query, job->onlyFolders);
        std::unordered_set<std::string> seen;
        for (const auto& local : pending.entries) {
            seen.insert(normalizeDedupKey(local.path));
        }

        if (resp.status == tools3000::core::ipc::SearchPipeStatus::Ready) {
            for (const auto& item : resp.items) {
                if (job->onlyFolders && !item.isDirectory) continue;
                const std::string key = normalizeDedupKey(item.path);
                if (key.empty() || !seen.insert(key).second) continue;
                InlineSearchEntry e;
                e.name = item.name;
                e.path = item.path;
                const std::filesystem::path fsP = tools3000::core::WinUtils::utf8ToPath(item.path).lexically_normal();
                e.parentDir = tools3000::core::WinUtils::pathToUtf8(fsP.parent_path());
                e.isDirectory = item.isDirectory;
                e.source = InlineSearchSource::DiskIndex;
                pending.entries.push_back(std::move(e));
                if (pending.entries.size() >= 40) break;
            }
        } else if (resp.status == tools3000::core::ipc::SearchPipeStatus::Starting) {
            pending.serviceStarting = true;
            const uint64_t expectedSeq = job->seq;
            HWND popupHwnd = nullptr;
            {
                std::lock_guard lock(m_mutex);
                popupHwnd = m_searchPopupHwnd;
            }
            tools3000::core::ipc::SearchPipeClient::instance().warmupAsync(
                [this, expectedSeq, popupHwnd](bool ready) {
                    if (ready && popupHwnd && IsWindow(popupHwnd) &&
                        m_searchWorkerRunning.load(std::memory_order_acquire) &&
                        m_searchQuerySeq.load(std::memory_order_acquire) == expectedSeq) {
                        PostMessageW(popupHwnd, WM_SEARCH_RETRY_AFTER_WARMUP, 0, 0);
                    }
                });
        } else if (resp.status != tools3000::core::ipc::SearchPipeStatus::Cancelled) {
            pending.serviceUnavailable = true;
        }

        {
            std::lock_guard lock(m_searchResultMutex);
            m_pendingSearchResult = std::move(pending);
        }

        HWND popupHwnd = nullptr;
        {
            std::lock_guard lock(m_mutex);
            popupHwnd = m_searchPopupHwnd;
        }
        if (popupHwnd && IsWindow(popupHwnd)) {
            PostMessageW(popupHwnd, WM_SEARCH_RESULTS_READY, 0, 0);
        }
    }
}

void DialogRibbonOverlay::applyPendingSearchResult() {
    std::optional<PendingSearchResult> res;
    {
        std::lock_guard lock(m_searchResultMutex);
        res = std::move(m_pendingSearchResult);
        m_pendingSearchResult.reset();
    }
    if (!res || !m_searchOpen) return;
    if (res->seq != m_searchQuerySeq.load(std::memory_order_acquire)) return;

    m_searchResults = std::move(res->entries);
    m_searchServiceStarting = res->serviceStarting;
    m_searchServiceUnavailable = res->serviceUnavailable;
    if (m_searchSelectedIndex >= static_cast<int>(m_searchResults.size())) {
        m_searchSelectedIndex = 0;
    }
    m_searchScrollTop = 0;
    layoutSearchPopup();
    renderSearchPopup();
}

void DialogRibbonOverlay::moveSearchSelection(int delta) {
    const int count = static_cast<int>(m_searchResults.size());
    if (count <= 0) return;
    m_searchSelectedIndex = (m_searchSelectedIndex + delta) % count;
    if (m_searchSelectedIndex < 0) m_searchSelectedIndex += count;
    if (m_searchSelectedIndex < m_searchScrollTop) {
        m_searchScrollTop = m_searchSelectedIndex;
    } else if (m_searchSelectedIndex >= m_searchScrollTop + kMaxVisibleSearchRows) {
        m_searchScrollTop = m_searchSelectedIndex - kMaxVisibleSearchRows + 1;
    }
    renderSearchPopup();
}

void DialogRibbonOverlay::toggleSearchScopeFilter() {
    m_searchOnlyFolders = !m_searchOnlyFolders;
    const bool isDark = tools3000::core::NativeThemePalette::isEffectiveDarkMode();
    float dpiScale = 1.0f;
    {
        std::lock_guard lock(m_mutex);
        dpiScale = m_dpiScale;
    }
    updateSearchEditVisuals(isDark, dpiScale);
    rebuildLocalSearchResults(m_searchCurrentQuery);
    if (tools3000::core::ipc::SearchPipeClient::shouldAllowAutoStart(m_searchCurrentQuery, true, true)) {
        dispatchDebouncedDiskSearch();
    }
}

void DialogRibbonOverlay::executeSelectedSearchResult(bool ctrlPressed, bool shiftPressed) {
    if (m_searchResults.empty()) {
        closeInlineSearchPopup(true);
        return;
    }
    const int idx = std::clamp(m_searchSelectedIndex, 0, static_cast<int>(m_searchResults.size()) - 1);
    const InlineSearchEntry chosen = m_searchResults[static_cast<size_t>(idx)];

    HWND targetDialog = nullptr;
    DWORD targetPid = 0;
    DialogMode targetMode = DialogMode::Unknown;
    {
        std::lock_guard lock(m_mutex);
        targetDialog = m_targetDialog;
        targetPid = m_targetProcessId;
        targetMode = m_targetDialogMode;
    }

    closeInlineSearchPopup(true);

    if (!targetDialog || !IsWindow(targetDialog)) return;
    DWORD currentPid = 0;
    GetWindowThreadProcessId(targetDialog, &currentPid);
    if (currentPid == 0 || currentPid != targetPid) return;

    const std::string dirToCache = chosen.isDirectory ? chosen.path : chosen.parentDir;
    DialogNavigator::instance().navigateToSearchResultAsync(
        targetDialog,
        chosen.path,
        chosen.isDirectory,
        targetMode,
        ctrlPressed,
        shiftPressed,
        [this, targetDialog, dirToCache](bool ok) {
            if (ok && !dirToCache.empty()) {
                setCachedDialogFolder(targetDialog, dirToCache);
            }
            updatePosition();
        });
}

void DialogRibbonOverlay::onSearchPopupMouseMove(int x, int y) {
    float dpiScale = 1.0f;
    {
        std::lock_guard lock(m_mutex);
        dpiScale = m_dpiScale;
    }
    const float fx = static_cast<float>(x);
    const float fy = static_cast<float>(y);

    bool dirty = false;
    const bool badgeInside =
        fx >= m_searchFilterBadgeRect.left && fx <= m_searchFilterBadgeRect.right &&
        fy >= m_searchFilterBadgeRect.top && fy <= m_searchFilterBadgeRect.bottom;
    if (badgeInside != m_searchFilterBadgeHovered) {
        m_searchFilterBadgeHovered = badgeInside;
        dirty = true;
    }

    const int headerH = static_cast<int>(std::lround(42.0f * dpiScale));
    const int rowH = static_cast<int>(std::lround(44.0f * dpiScale));
    int newHover = -1;
    if (y >= headerH) {
        const int relRow = (y - headerH) / (std::max)(1, rowH);
        const int absIdx = m_searchScrollTop + relRow;
        if (relRow >= 0 && relRow < kMaxVisibleSearchRows &&
            absIdx >= 0 && absIdx < static_cast<int>(m_searchResults.size())) {
            newHover = absIdx;
        }
    }
    if (newHover != m_searchHoveredIndex) {
        m_searchHoveredIndex = newHover;
        if (newHover >= 0) {
            m_searchSelectedIndex = newHover;
        }
        dirty = true;
    }
    if (dirty) renderSearchPopup();
}

void DialogRibbonOverlay::onSearchPopupLButtonDown(int x, int y) {
    const float fx = static_cast<float>(x);
    const float fy = static_cast<float>(y);
    if (fx >= m_searchFilterBadgeRect.left && fx <= m_searchFilterBadgeRect.right &&
        fy >= m_searchFilterBadgeRect.top && fy <= m_searchFilterBadgeRect.bottom) {
        toggleSearchScopeFilter();
        if (m_searchEditHwnd && IsWindow(m_searchEditHwnd)) {
            SetFocus(m_searchEditHwnd);
        }
        return;
    }

    onSearchPopupMouseMove(x, y);
    if (m_searchHoveredIndex >= 0 && m_searchHoveredIndex < static_cast<int>(m_searchResults.size())) {
        m_searchSelectedIndex = m_searchHoveredIndex;
        const bool ctrlDown = (GetKeyState(VK_CONTROL) & 0x8000) != 0;
        const bool shiftDown = (GetKeyState(VK_SHIFT) & 0x8000) != 0;
        executeSelectedSearchResult(ctrlDown, shiftDown);
    }
}

void DialogRibbonOverlay::onSearchPopupMouseWheel(short wheelDelta) {
    const int count = static_cast<int>(m_searchResults.size());
    if (count <= kMaxVisibleSearchRows) return;
    const int step = wheelDelta > 0 ? -1 : 1;
    const int maxScroll = count - kMaxVisibleSearchRows;
    const int nextScroll = std::clamp(m_searchScrollTop + step, 0, maxScroll);
    if (nextScroll != m_searchScrollTop) {
        m_searchScrollTop = nextScroll;
        renderSearchPopup();
    }
}

void DialogRibbonOverlay::renderSearchPopup() {
    if (!m_searchPopupHwnd || !IsWindow(m_searchPopupHwnd) || !m_searchOpen) return;
    InvalidateRect(m_searchPopupHwnd, nullptr, FALSE);
}

// ============================================================
// 主胶囊 WndProc + 内嵌搜索浮层 WndProc + Edit 子类化按键拦截
// ============================================================
LRESULT CALLBACK DialogRibbonOverlay::SearchEditSubclassProc(
    HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam, UINT_PTR, DWORD_PTR refData) {
    auto* self = reinterpret_cast<DialogRibbonOverlay*>(refData);
    if (self && msg == WM_KEYDOWN) {
        if (wParam == VK_DOWN) {
            self->moveSearchSelection(1);
            return 0;
        }
        if (wParam == VK_UP) {
            self->moveSearchSelection(-1);
            return 0;
        }
        if (wParam == VK_NEXT) {
            self->moveSearchSelection(4);
            return 0;
        }
        if (wParam == VK_PRIOR) {
            self->moveSearchSelection(-4);
            return 0;
        }
        if (wParam == VK_RETURN) {
            const bool ctrlDown = (GetKeyState(VK_CONTROL) & 0x8000) != 0;
            const bool shiftDown = (GetKeyState(VK_SHIFT) & 0x8000) != 0;
            self->executeSelectedSearchResult(ctrlDown, shiftDown);
            return 0;
        }
        if (wParam == VK_ESCAPE) {
            self->closeInlineSearchPopup(true);
            return 0;
        }
        if (wParam == VK_TAB) {
            self->toggleSearchScopeFilter();
            return 0;
        }
    }
    if (msg == WM_CHAR && (wParam == VK_RETURN || wParam == VK_ESCAPE || wParam == VK_TAB)) {
        // 阻止单行 Edit 发系统提示嘟声
        return 0;
    }
    return DefSubclassProc(hwnd, msg, wParam, lParam);
}

LRESULT CALLBACK DialogRibbonOverlay::SearchPopupWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    DialogRibbonOverlay* self = nullptr;
    if (msg == WM_CREATE) {
        auto* cs = reinterpret_cast<CREATESTRUCTW*>(lParam);
        self = reinterpret_cast<DialogRibbonOverlay*>(cs->lpCreateParams);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
    } else {
        self = reinterpret_cast<DialogRibbonOverlay*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    }

    if (self) {
        switch (msg) {
            case WM_ERASEBKGND:
                return 1;
            case WM_CTLCOLOREDIT: {
                HDC hdc = reinterpret_cast<HDC>(wParam);
                SetTextColor(hdc, self->m_searchEditTextColor);
                SetBkColor(hdc, self->m_searchEditBgColor);
                return reinterpret_cast<LRESULT>(self->m_searchEditBgBrush);
            }
            case WM_COMMAND:
                if (LOWORD(wParam) == kSearchEditControlId && HIWORD(wParam) == EN_CHANGE) {
                    self->onSearchQueryTextChanged();
                    return 0;
                }
                break;
            case WM_TIMER:
                if (wParam == kSearchDebounceTimerId) {
                    KillTimer(hwnd, kSearchDebounceTimerId);
                    self->dispatchDebouncedDiskSearch();
                    return 0;
                }
                break;
            case WM_SEARCH_RESULTS_READY:
                self->applyPendingSearchResult();
                return 0;
            case WM_SEARCH_RETRY_AFTER_WARMUP:
                if (self->m_searchOpen) {
                    self->dispatchDebouncedDiskSearch();
                }
                return 0;
            case WM_ACTIVATE:
                if (LOWORD(wParam) == WA_INACTIVE) {
                    HWND nextActive = reinterpret_cast<HWND>(lParam);
                    if (!self->isOwnOverlayWindow(nextActive)) {
                        self->closeInlineSearchPopup(false);
                    }
                }
                return 0;
            case WM_MOUSEMOVE:
                self->onSearchPopupMouseMove(
                    static_cast<int>(static_cast<short>(LOWORD(lParam))),
                    static_cast<int>(static_cast<short>(HIWORD(lParam))));
                return 0;
            case WM_LBUTTONDOWN:
                self->onSearchPopupLButtonDown(
                    static_cast<int>(static_cast<short>(LOWORD(lParam))),
                    static_cast<int>(static_cast<short>(HIWORD(lParam))));
                return 0;
            case WM_MOUSEWHEEL:
                self->onSearchPopupMouseWheel(GET_WHEEL_DELTA_WPARAM(wParam));
                return 0;
            case WM_PAINT: {
                PAINTSTRUCT ps{};
                HDC paintDC = BeginPaint(hwnd, &ps);
                RECT clientRc{};
                GetClientRect(hwnd, &clientRc);
                const int w = clientRc.right - clientRc.left;
                const int h = clientRc.bottom - clientRc.top;

                if (w > 0 && h > 0 && self->m_d2dFactory) {
                    float dpiScale = 1.0f;
                    DialogMode mode = DialogMode::Unknown;
                    {
                        std::lock_guard lock(self->m_mutex);
                        dpiScale = self->m_dpiScale;
                        mode = self->m_targetDialogMode;
                    }
                    self->ensureTextFormats(dpiScale);

                    HDC bufDC = CreateCompatibleDC(paintDC);
                    HBITMAP bufBmp = CreateCompatibleBitmap(paintDC, w, h);
                    HBITMAP oldBmp = bufDC && bufBmp ? static_cast<HBITMAP>(SelectObject(bufDC, bufBmp)) : nullptr;

                    if (!self->m_searchRenderTarget) {
                        D2D1_RENDER_TARGET_PROPERTIES props = D2D1::RenderTargetProperties(
                            D2D1_RENDER_TARGET_TYPE_DEFAULT,
                            D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_IGNORE),
                            96.0f, 96.0f,
                            D2D1_RENDER_TARGET_USAGE_GDI_COMPATIBLE);
                        if (SUCCEEDED(self->m_d2dFactory->CreateDCRenderTarget(
                                &props, self->m_searchRenderTarget.GetAddressOf())) &&
                            self->m_searchRenderTarget) {
                            self->m_searchRenderTarget->SetDpi(96.0f, 96.0f);
                        }
                    }

                    if (bufDC && bufBmp && self->m_searchRenderTarget &&
                        SUCCEEDED(self->m_searchRenderTarget->BindDC(bufDC, &clientRc))) {
                        auto* rt = self->m_searchRenderTarget.Get();
                        const bool isDark = tools3000::core::NativeThemePalette::isEffectiveDarkMode();
                        const auto accent = tools3000::core::NativeThemePalette::getEffectiveAccentColorRGB();

                        ComPtr<ID2D1SolidColorBrush> bgBrush, headerBrush, borderBrush, textBrush, mutedBrush,
                            accentBrush, rowHoverBrush, tagBgBrush, footerBgBrush;
                        rt->CreateSolidColorBrush(
                            isDark ? D2D1::ColorF(0.102f, 0.114f, 0.149f, 1.0f)
                                   : D2D1::ColorF(0.973f, 0.980f, 0.988f, 1.0f),
                            bgBrush.GetAddressOf());
                        rt->CreateSolidColorBrush(
                            isDark ? D2D1::ColorF(0.118f, 0.130f, 0.172f, 1.0f)
                                   : D2D1::ColorF(1.0f, 1.0f, 1.0f, 1.0f),
                            headerBrush.GetAddressOf());
                        rt->CreateSolidColorBrush(
                            isDark ? D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.12f)
                                   : D2D1::ColorF(0.0f, 0.0f, 0.0f, 0.10f),
                            borderBrush.GetAddressOf());
                        rt->CreateSolidColorBrush(
                            isDark ? D2D1::ColorF(0.95f, 0.96f, 0.98f, 1.0f)
                                   : D2D1::ColorF(0.09f, 0.11f, 0.15f, 1.0f),
                            textBrush.GetAddressOf());
                        rt->CreateSolidColorBrush(
                            isDark ? D2D1::ColorF(0.62f, 0.67f, 0.75f, 1.0f)
                                   : D2D1::ColorF(0.38f, 0.43f, 0.51f, 1.0f),
                            mutedBrush.GetAddressOf());
                        rt->CreateSolidColorBrush(
                            D2D1::ColorF(accent.r, accent.g, accent.b, 1.0f),
                            accentBrush.GetAddressOf());
                        rt->CreateSolidColorBrush(
                            isDark ? D2D1::ColorF(accent.r, accent.g, accent.b, 0.22f)
                                   : D2D1::ColorF(accent.r, accent.g, accent.b, 0.14f),
                            rowHoverBrush.GetAddressOf());
                        rt->CreateSolidColorBrush(
                            isDark ? D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.09f)
                                   : D2D1::ColorF(0.0f, 0.0f, 0.0f, 0.06f),
                            tagBgBrush.GetAddressOf());
                        rt->CreateSolidColorBrush(
                            isDark ? D2D1::ColorF(0.082f, 0.092f, 0.122f, 1.0f)
                                   : D2D1::ColorF(0.937f, 0.949f, 0.965f, 1.0f),
                            footerBgBrush.GetAddressOf());

                        rt->BeginDraw();
                        rt->FillRectangle(D2D1::RectF(0, 0, static_cast<float>(w), static_cast<float>(h)), bgBrush.Get());

                        const float headerH = 42.0f * dpiScale;
                        const float rowH = 44.0f * dpiScale;
                        const float footerH = 26.0f * dpiScale;

                        // 顶部搜索输入栏背景 + 分割线
                        rt->FillRectangle(D2D1::RectF(0, 0, static_cast<float>(w), headerH), headerBrush.Get());
                        rt->DrawLine(D2D1::Point2F(0, headerH), D2D1::Point2F(static_cast<float>(w), headerH),
                                     borderBrush.Get(), 1.0f * dpiScale);

                        // 左侧放大镜矢量图标
                        const float searchIconSz = 14.0f * dpiScale;
                        self->drawSearchIcon(rt, accentBrush.Get(),
                                             13.0f * dpiScale, (headerH - searchIconSz) * 0.5f,
                                             searchIconSz, 1.5f * dpiScale);

                        // 右侧范围切换胶囊徽章 ([仅文件夹] / [全部])
                        const float badgeW = 72.0f * dpiScale;
                        const float badgeH = 22.0f * dpiScale;
                        self->m_searchFilterBadgeRect = D2D1::RectF(
                            static_cast<float>(w) - badgeW - 10.0f * dpiScale,
                            (headerH - badgeH) * 0.5f,
                            static_cast<float>(w) - 10.0f * dpiScale,
                            (headerH + badgeH) * 0.5f);
                        const auto badgeRRect = D2D1::RoundedRect(self->m_searchFilterBadgeRect, 6.0f * dpiScale, 6.0f * dpiScale);
                        rt->FillRoundedRectangle(&badgeRRect,
                            self->m_searchOnlyFolders ? rowHoverBrush.Get() : tagBgBrush.Get());
                        rt->DrawRoundedRectangle(&badgeRRect,
                            (self->m_searchOnlyFolders || self->m_searchFilterBadgeHovered)
                                ? accentBrush.Get() : borderBrush.Get(),
                            1.0f * dpiScale);
                        if (self->m_searchMetaFormat) {
                            const std::string scopeStr = self->m_searchOnlyFolders
                                ? tools3000::core::trUtf8("仅文件夹", "Folders")
                                : tools3000::core::trUtf8("全部", "All");
                            const std::wstring wScope = tools3000::core::WinUtils::utf8ToWstring(scopeStr);
                            self->m_searchMetaFormat->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
                            self->m_searchMetaFormat->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
                            rt->DrawText(wScope.c_str(), static_cast<UINT32>(wScope.size()),
                                         self->m_searchMetaFormat.Get(), self->m_searchFilterBadgeRect,
                                         self->m_searchOnlyFolders ? accentBrush.Get() : textBrush.Get());
                        }

                        // 中部结果列表区域
                        const float listTop = headerH + 4.0f * dpiScale;
                        if (self->m_searchResults.empty()) {
                            if (self->m_searchPathFormat) {
                                const std::string emptyMsg = self->m_searchServiceStarting
                                    ? tools3000::core::trUtf8("正在按需启动全盘索引...", "Starting full-disk index on demand...")
                                    : tools3000::core::trUtf8("未找到匹配项", "No matching items");
                                const std::wstring wEmpty = tools3000::core::WinUtils::utf8ToWstring(emptyMsg);
                                self->m_searchPathFormat->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
                                self->m_searchPathFormat->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
                                rt->DrawText(wEmpty.c_str(), static_cast<UINT32>(wEmpty.size()),
                                             self->m_searchPathFormat.Get(),
                                             D2D1::RectF(12.0f * dpiScale, listTop,
                                                         static_cast<float>(w) - 12.0f * dpiScale, listTop + rowH),
                                             mutedBrush.Get());
                            }
                        } else {
                            const int total = static_cast<int>(self->m_searchResults.size());
                            const int endIdx = (std::min)(total, self->m_searchScrollTop + kMaxVisibleSearchRows);
                            for (int idx = self->m_searchScrollTop; idx < endIdx; ++idx) {
                                const auto& item = self->m_searchResults[static_cast<size_t>(idx)];
                                const int visRow = idx - self->m_searchScrollTop;
                                const float rowY = listTop + static_cast<float>(visRow) * rowH;
                                const bool selected = (idx == self->m_searchSelectedIndex);

                                D2D1_RECT_F rowRect = D2D1::RectF(
                                    6.0f * dpiScale, rowY + 1.0f * dpiScale,
                                    static_cast<float>(w) - 6.0f * dpiScale, rowY + rowH - 1.0f * dpiScale);
                                if (selected) {
                                    const auto selRR = D2D1::RoundedRect(rowRect, 6.0f * dpiScale, 6.0f * dpiScale);
                                    rt->FillRoundedRectangle(&selRR, rowHoverBrush.Get());
                                    // 左侧强调色选中竖条
                                    const auto barRR = D2D1::RoundedRect(
                                        D2D1::RectF(rowRect.left + 2.0f * dpiScale, rowRect.top + 7.0f * dpiScale,
                                                    rowRect.left + 5.0f * dpiScale, rowRect.bottom - 7.0f * dpiScale),
                                        1.5f * dpiScale, 1.5f * dpiScale);
                                    rt->FillRoundedRectangle(&barRR, accentBrush.Get());
                                }

                                // 图标
                                const float iconSz = 15.0f * dpiScale;
                                const float iconX = rowRect.left + 12.0f * dpiScale;
                                const float iconY = rowRect.top + (rowH - 2.0f * dpiScale - iconSz) * 0.5f;
                                ID2D1Brush* iconBr = item.isDirectory ? accentBrush.Get() : mutedBrush.Get();
                                if (item.source == InlineSearchSource::Favorite) {
                                    self->drawBookmarkIcon(rt, iconBr, iconX, iconY, iconSz, 1.35f * dpiScale);
                                } else if (item.source == InlineSearchSource::Recent) {
                                    self->drawClockIcon(rt, iconBr, iconX, iconY, iconSz, 1.35f * dpiScale);
                                } else if (item.isDirectory) {
                                    self->drawFolderIcon(rt, iconBr, iconX, iconY, iconSz, 1.35f * dpiScale);
                                } else {
                                    self->drawFileIcon(rt, iconBr, iconX, iconY, iconSz, 1.35f * dpiScale);
                                }

                                // 右侧来源标签
                                std::string tagStr;
                                switch (item.source) {
                                    case InlineSearchSource::ExplorerTab:
                                        tagStr = tools3000::core::trUtf8("资源管理器", "Explorer");
                                        break;
                                    case InlineSearchSource::Favorite:
                                        tagStr = tools3000::core::trUtf8("收藏", "Workspace");
                                        break;
                                    case InlineSearchSource::AppMemory:
                                        tagStr = tools3000::core::trUtf8("应用记忆", "Memory");
                                        break;
                                    case InlineSearchSource::Recent:
                                        tagStr = tools3000::core::trUtf8("最近目录", "Recent Folder");
                                        break;
                                    case InlineSearchSource::DiskIndex:
                                    default:
                                        tagStr = item.isDirectory
                                            ? tools3000::core::trUtf8("目录", "Folder")
                                            : tools3000::core::trUtf8("文件", "File");
                                        break;
                                }
                                const float tagW = 68.0f * dpiScale;
                                const float tagH = 18.0f * dpiScale;
                                D2D1_RECT_F tagRect = D2D1::RectF(
                                    rowRect.right - tagW - 8.0f * dpiScale,
                                    rowRect.top + 5.0f * dpiScale,
                                    rowRect.right - 8.0f * dpiScale,
                                    rowRect.top + 5.0f * dpiScale + tagH);
                                rt->FillRoundedRectangle(D2D1::RoundedRect(tagRect, 4.0f * dpiScale, 4.0f * dpiScale), tagBgBrush.Get());
                                if (self->m_searchMetaFormat) {
                                    const std::wstring wTag = tools3000::core::WinUtils::utf8ToWstring(tagStr);
                                    self->m_searchMetaFormat->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
                                    self->m_searchMetaFormat->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
                                    rt->DrawText(wTag.c_str(), static_cast<UINT32>(wTag.size()),
                                                 self->m_searchMetaFormat.Get(), tagRect, mutedBrush.Get());
                                }

                                // 名称与父路径双行排版
                                const float textLeft = iconX + iconSz + 10.0f * dpiScale;
                                const float titleRight = tagRect.left - 8.0f * dpiScale;
                                const float pathRight = rowRect.right - 10.0f * dpiScale;

                                if (self->m_searchTitleFormat) {
                                    const std::wstring wName = tools3000::core::WinUtils::utf8ToWstring(item.name);
                                    self->m_searchTitleFormat->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
                                    self->m_searchTitleFormat->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
                                    rt->DrawText(wName.c_str(), static_cast<UINT32>(wName.size()),
                                                 self->m_searchTitleFormat.Get(),
                                                 D2D1::RectF(textLeft, rowRect.top + 3.0f * dpiScale,
                                                             titleRight, rowRect.top + 22.0f * dpiScale),
                                                 textBrush.Get());
                                }
                                if (self->m_searchPathFormat) {
                                    const std::wstring wSub = tools3000::core::WinUtils::utf8ToWstring(
                                        item.isDirectory ? item.path : item.parentDir);
                                    self->m_searchPathFormat->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
                                    self->m_searchPathFormat->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
                                    rt->DrawText(wSub.c_str(), static_cast<UINT32>(wSub.size()),
                                                 self->m_searchPathFormat.Get(),
                                                 D2D1::RectF(textLeft, rowRect.top + 22.0f * dpiScale,
                                                             pathRight, rowRect.bottom - 2.0f * dpiScale),
                                                 mutedBrush.Get());
                                }
                            }
                        }

                        // 底部状态提示栏
                        const float footerTop = static_cast<float>(h) - footerH;
                        rt->FillRectangle(D2D1::RectF(0, footerTop, static_cast<float>(w), static_cast<float>(h)),
                                          footerBgBrush.Get());
                        rt->DrawLine(D2D1::Point2F(0, footerTop), D2D1::Point2F(static_cast<float>(w), footerTop),
                                     borderBrush.Get(), 1.0f * dpiScale);

                        std::string hint;
                        if (self->m_searchServiceStarting) {
                            hint = tools3000::core::trUtf8("正在按需启动全盘索引...", "Starting full-disk index on demand...");
                        } else if (self->m_searchServiceUnavailable) {
                            hint = tools3000::core::trUtf8("全盘索引服务暂不可用（仅显示本地候选）", "Index service unavailable (local candidates only)");
                        } else if (mode == DialogMode::SaveFile) {
                            hint = tools3000::core::trUtf8("另存为安全模式：强制仅跳转父目录 · Tab 切换 · Esc 关闭", "Save As Safe Mode: Folder Only · Tab Scope · Esc Close");
                        } else if (mode == DialogMode::OpenFile &&
                                   !self->m_searchResults.empty() &&
                                   self->m_searchSelectedIndex >= 0 &&
                                   self->m_searchSelectedIndex < static_cast<int>(self->m_searchResults.size()) &&
                                   !self->m_searchResults[static_cast<size_t>(self->m_searchSelectedIndex)].isDirectory) {
                            hint = tools3000::core::trUtf8("Enter 填入文件名 · Shift+Enter 打开 · Ctrl+Enter 仅目录 · Esc 关闭", "Enter Fill Name · Shift+Enter Open · Ctrl+Enter Folder Only · Esc Close");
                        } else {
                            hint = tools3000::core::trUtf8("Enter 跳转 · Tab 切换范围 · Ctrl+Enter 仅目录 · Esc 关闭", "Enter Jump · Tab Scope · Ctrl+Enter Folder Only · Esc Close");
                        }

                        if (self->m_searchMetaFormat) {
                            const std::wstring wHint = tools3000::core::WinUtils::utf8ToWstring(hint);
                            self->m_searchMetaFormat->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
                            self->m_searchMetaFormat->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
                            rt->DrawText(wHint.c_str(), static_cast<UINT32>(wHint.size()),
                                         self->m_searchMetaFormat.Get(),
                                         D2D1::RectF(12.0f * dpiScale, footerTop,
                                                     static_cast<float>(w) - 12.0f * dpiScale, static_cast<float>(h)),
                                         self->m_searchServiceStarting ? accentBrush.Get() : mutedBrush.Get());
                        }

                        // 外框精细描边
                        rt->DrawRectangle(
                            D2D1::RectF(0.5f, 0.5f, static_cast<float>(w) - 0.5f, static_cast<float>(h) - 0.5f),
                            borderBrush.Get(), 1.0f);

                        if (FAILED(rt->EndDraw())) {
                            self->discardSearchDeviceResources();
                        }

                        BitBlt(paintDC, 0, 0, w, h, bufDC, 0, 0, SRCCOPY);
                    }

                    if (oldBmp) SelectObject(bufDC, oldBmp);
                    if (bufBmp) DeleteObject(bufBmp);
                    if (bufDC) DeleteDC(bufDC);
                }

                EndPaint(hwnd, &ps);
                return 0;
            }
            default:
                break;
        }
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

LRESULT CALLBACK DialogRibbonOverlay::WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    DialogRibbonOverlay* self = nullptr;
    if (msg == WM_CREATE) {
        auto* cs = reinterpret_cast<CREATESTRUCTW*>(lParam);
        self = reinterpret_cast<DialogRibbonOverlay*>(cs->lpCreateParams);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
    } else {
        self = reinterpret_cast<DialogRibbonOverlay*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    }

    if (self) {
        switch (msg) {
            case WM_HOTKEY:
                if (static_cast<int>(wParam) == kInlineSearchHotkeyId &&
                    PathMemoryManager::instance().isInlineSearchEnabled()) {
                    const HWND fg = GetForegroundWindow();
                    const HWND fgRoot = fg ? GetAncestor(fg, GA_ROOT) : nullptr;
                    const HWND target = self->getTargetDialog();
                    if (target && IsWindow(target) &&
                        (fg == target || fgRoot == target ||
                         self->isOwnOverlayWindow(fg) || self->isOwnOverlayWindow(fgRoot))) {
                        if (self->m_searchOpen) {
                            self->closeInlineSearchPopup(true);
                        } else {
                            self->openInlineSearchPopup();
                        }
                    } else {
                        self->syncInlineSearchHotkey(false);
                    }
                }
                return 0;
            case WM_RIBBON_TOGGLE_SEARCH:
                if (self->m_searchOpen) {
                    self->closeInlineSearchPopup(true);
                } else {
                    self->openInlineSearchPopup();
                }
                return 0;
            case WM_RIBBON_UPDATE_POS:
                self->m_posUpdatePending.store(false, std::memory_order_release);
                self->doUpdatePosition();
                return 0;
            case WM_TIMER:
            case WM_DPICHANGED:
                self->doUpdatePosition();
                return 0;
            case WM_RIBBON_ATTACH:
                self->doAttachToDialog();
                return 0;
            case WM_RIBBON_REFRESH_CONTENT: {
                {
                    std::lock_guard lock(self->m_mutex);
                    if (wParam != 0) {
                        self->m_dismissedDialog = nullptr;
                    }
                    self->m_buttons.clear();
                    self->m_renderedWidth = 0;
                    self->m_renderedHeight = 0;
                    self->m_d2dConsecutiveFailures = 0;
                    self->m_d2dRetryAfterTick = 0;
                }
                self->doUpdatePosition();
                return 0;
            }
            case WM_RIBBON_THEME_CHANGED: {
                self->m_brushesValid = false;
                self->discardSearchDeviceResources();
                HWND tooltipHwnd = nullptr;
                HWND searchPopup = nullptr;
                bool hasTarget = false;
                float dpiScale = 1.0f;
                {
                    std::lock_guard lock(self->m_mutex);
                    tooltipHwnd = self->m_tooltipHwnd;
                    searchPopup = self->m_searchPopupHwnd;
                    self->m_renderedWidth = 0;
                    self->m_renderedHeight = 0;
                    self->m_d2dConsecutiveFailures = 0;
                    self->m_d2dRetryAfterTick = 0;
                    hasTarget = (self->m_targetDialog != nullptr);
                    dpiScale = self->m_dpiScale;
                }
                const bool isDark = tools3000::core::NativeThemePalette::isEffectiveDarkMode();
                if (tooltipHwnd && IsWindow(tooltipHwnd)) {
                    SetWindowTheme(tooltipHwnd, isDark ? L"DarkMode_Explorer" : L"Explorer", nullptr);
                }
                if (searchPopup && IsWindow(searchPopup)) {
                    tools3000::core::WinUtils::applyImmersiveDarkMode(searchPopup, isDark);
                    self->updateSearchEditVisuals(isDark, dpiScale);
                    if (self->m_searchOpen) self->renderSearchPopup();
                }
                if (hasTarget) {
                    self->doUpdatePosition();
                }
                return 0;
            }
            case WM_RIBBON_HIDE:
                self->doHide();
                return 0;
            case WM_MOUSEMOVE:
                self->onMouseMove(
                    static_cast<int>(static_cast<short>(LOWORD(lParam))),
                    static_cast<int>(static_cast<short>(HIWORD(lParam))));
                return 0;
            case WM_MOUSELEAVE:
                self->onMouseLeave();
                return 0;
            case WM_LBUTTONDOWN:
                self->onLButtonDown(
                    static_cast<int>(static_cast<short>(LOWORD(lParam))),
                    static_cast<int>(static_cast<short>(HIWORD(lParam))));
                return 0;
            case WM_SETCURSOR:
                SetCursor(LoadCursorW(nullptr, IDC_HAND));
                return TRUE;
            default:
                break;
        }
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

}  // namespace tools3000::dialog
