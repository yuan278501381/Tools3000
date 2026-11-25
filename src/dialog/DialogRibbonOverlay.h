/**
 * Tools3000 - High Performance Windows Productivity Suite
 *
 * Copyright (c) 2026 Yy1 (GitHub yuan278501381) <https://github.com/yuan278501381> & Tools3000 contributors
 *
 * Licensed under the GPL-3.0 License.
 */

#pragma once

#include "DialogEventAndLayoutPolicy.h"
#include "core/i18n/UiI18n.h"
#include "core/utils/ThemeUtils.h"

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>
#include <windows.h>
#include <shobjidl_core.h>
#include <d2d1.h>
#include <dwrite.h>
#include <wrl/client.h>

namespace tools3000::dialog {

struct RibbonButtonRect {
    enum class Type { Search, ExplorerSync, Recent, Favorites, Close };
    Type type;
    D2D1_RECT_F rect;
    std::string text;
    std::string extraData;
    int badgeCount{0};
    bool hovered{false};
    float textWidth{0.0f};
    float badgeWidth{0.0f};
};

enum class InlineSearchSource : uint8_t {
    ExplorerTab = 0,
    Favorite = 1,
    AppMemory = 2,
    Recent = 3,
    DiskIndex = 4,
};

struct InlineSearchEntry {
    std::string name;
    std::string path;
    std::string parentDir;
    bool isDirectory{true};
    InlineSearchSource source{InlineSearchSource::DiskIndex};
};

class DialogRibbonOverlay {
public:
    static DialogRibbonOverlay& instance();

    bool init();
    void cleanup();

    // 绑定并展示在指定文件对话框顶部/底部（支持从任意后台线程安全调用，零主线程跨进程阻塞）
    void attachToDialog(HWND dialogHwnd, const std::string& processName);
    void attachToDialog(HWND dialogHwnd,
                        const std::string& processName,
                        DialogArchitecture architecture,
                        DialogMode mode,
                        const std::string& initialFolder);
    void setCachedDialogFolder(HWND dialogHwnd, const std::string& folderPath);
    void updatePosition();
    void refreshContent(bool clearDismissed = false);
    void onThemeChanged();
    void hide();

    // 判断指定句柄是否属于胶囊或内嵌搜索浮层自身（避免焦点归属误隐藏）
    bool isOwnOverlayWindow(HWND hwnd) const noexcept;

    // 展开/收起 Listary 风格内嵌极速搜索框
    void toggleInlineSearch();

    // 支持脱离悬浮胶囊条（ribbonEnabled=false）直接按用户自定义快捷键为当前文件对话框呼出/收起内嵌搜索
    void toggleInlineSearchForDialog(HWND dialogHwnd,
                                     const std::string& processName,
                                     DialogArchitecture architecture,
                                     DialogMode mode,
                                     const std::string& cachedFolder);

    static bool computeSafeHorizontalPlacement(const RECT& dlgRect, float dpiScale,
                                               int ribbonWidth, const std::string& posConfig,
                                               int& outPosX, int& outMaxAvailableWidth) noexcept;

    HWND getHwnd() const { std::lock_guard lock(m_mutex); return m_hwnd; }
    HWND getTargetDialog() const { std::lock_guard lock(m_mutex); return m_targetDialog; }

private:
    DialogRibbonOverlay();
    ~DialogRibbonOverlay();

    struct SearchWorkerJob {
        uint64_t seq{0};
        std::string query;
        bool onlyFolders{false};
        std::string currentFolder;
        std::string targetProcess;
    };

    struct PendingSearchResult {
        uint64_t seq{0};
        std::vector<InlineSearchEntry> entries;
        bool serviceStarting{false};
        bool serviceUnavailable{false};
        int64_t elapsedMs{0};
    };

    static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);
    static LRESULT CALLBACK SearchPopupWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);
    static LRESULT CALLBACK SearchEditSubclassProc(
        HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam, UINT_PTR subclassId, DWORD_PTR refData);

    void cleanupOnUiThread();
    void attachValidatedOnUiThread(HWND dialogHwnd,
                                   DWORD processId,
                                   const std::string& processName,
                                   DialogArchitecture architecture,
                                   DialogMode mode,
                                   const std::string& initialFolder);
    bool doUpdatePosition();
    void doAttachToDialog();
    void doHide();
    void resetStateLocked();
    void discardDeviceResources();
    void discardSearchDeviceResources();
    bool ensureDeviceBrushes(bool isDark, const tools3000::core::AccentColorRGB& accent);
    void render();
    void updateButtonsLayout(float dpiScale, int maxAvailableWidth = 0);
    bool hitTestButton(const RibbonButtonRect& btn, int x, int y, float dpiScale) const noexcept;
    void onLButtonDown(int x, int y);
    void onMouseMove(int x, int y);
    void onMouseLeave();
    void updateTooltipText(const std::string& utf8Tip);
    void showRecentMenu(int screenX, int screenY, bool rightAlign = false);
    void showFavoritesMenu(int screenX, int screenY, bool rightAlign = false);
    void ensureTextFormats(float dpiScale);
    void syncInlineSearchHotkey(bool wantRegistered);
    void applyRelativeZOrder(HWND overlayHwnd, HWND targetDialog, int x, int y, int w, int h, UINT extraFlags);

    // ── 内嵌极速搜索浮层方法 ────────────────────────────────────────────────
    bool ensureSearchPopupCreated();
    void openInlineSearchPopup();
    void closeInlineSearchPopup(bool restoreTargetFocus);
    void layoutSearchPopup();
    void updateSearchEditVisuals(bool isDark, float dpiScale);
    void rebuildLocalSearchResults(const std::string& queryUtf8);
    std::vector<InlineSearchEntry> collectLocalCandidates(const std::string& queryUtf8, bool onlyFolders) const;
    void onSearchQueryTextChanged();
    void dispatchDebouncedDiskSearch();
    void startSearchWorker();
    void stopSearchWorker();
    void searchWorkerLoop();
    void applyPendingSearchResult();
    void renderSearchPopup();
    void moveSearchSelection(int delta);
    void toggleSearchScopeFilter();
    void executeSelectedSearchResult(bool ctrlPressed, bool shiftPressed);
    void onSearchPopupMouseMove(int x, int y);
    void onSearchPopupLButtonDown(int x, int y);
    void onSearchPopupMouseWheel(short wheelDelta);

    // ── 高精度矢量图标绘制 ─────────────────────────────────────────────────
    void drawSearchIcon(ID2D1RenderTarget* rt, ID2D1Brush* brush, float x, float y, float size, float strokeWidth);
    void drawFolderIcon(ID2D1RenderTarget* rt, ID2D1Brush* brush, float x, float y, float size, float strokeWidth);
    void drawFileIcon(ID2D1RenderTarget* rt, ID2D1Brush* brush, float x, float y, float size, float strokeWidth);
    void drawClockIcon(ID2D1RenderTarget* rt, ID2D1Brush* brush, float x, float y, float size, float strokeWidth);
    void drawBookmarkIcon(ID2D1RenderTarget* rt, ID2D1Brush* brush, float x, float y, float size, float strokeWidth);
    void drawChevron(ID2D1RenderTarget* rt, ID2D1Brush* brush, float centerX, float centerY, float size, float strokeWidth);
    void drawCloseIcon(ID2D1RenderTarget* rt, ID2D1Brush* brush, float centerX, float centerY, float size, float strokeWidth);
    void drawBadge(ID2D1RenderTarget* rt, ID2D1Brush* bgBrush, ID2D1Brush* textBrush, float x, float y, float w, float h, int count);

    HWND m_hwnd{nullptr};
    HWND m_tooltipHwnd{nullptr};
    HWND m_searchPopupHwnd{nullptr};
    HWND m_searchEditHwnd{nullptr};
    HWND m_targetDialog{nullptr};
    HWND m_dismissedDialog{nullptr};
    HWND m_cachedFolderOwner{nullptr};
    DWORD m_targetProcessId{0};
    DialogArchitecture m_targetArchitecture{DialogArchitecture::Unknown};
    DialogMode m_targetDialogMode{DialogMode::Unknown};
    std::string m_targetProcess;
    std::string m_cachedDialogFolder;
    std::string m_activeExplorerPath;
    std::string m_currentTooltipText;
    std::atomic<bool> m_posUpdatePending{false};
    bool m_hotkeyRegistered{false};
    Microsoft::WRL::ComPtr<IVirtualDesktopManager> m_vdm;
    bool m_vdmInitialized{false};

    int m_width{360};
    int m_height{25};
    int m_posX{0};
    int m_posY{0};
    int m_lastMaxAvailableWidth{0};
    float m_dpiScale{1.0f};
    float m_currentFontDpi{0.0f};
    tools3000::core::UiLanguage m_currentUiLang{tools3000::core::UiLanguage::ZhCN};

    // D2D EndDraw 失败退避计数（最多连续重试 3 次，杜绝死循环）
    int m_d2dConsecutiveFailures{0};
    ULONGLONG m_d2dRetryAfterTick{0};

    Microsoft::WRL::ComPtr<ID2D1Factory> m_d2dFactory;
    Microsoft::WRL::ComPtr<ID2D1DCRenderTarget> m_renderTarget;
    Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> m_bgBrush;
    Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> m_borderBrush;
    Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> m_textBrush;
    Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> m_textMutedBrush;
    Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> m_highlightBrush;
    Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> m_highlightBorderBrush;
    Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> m_syncBgBrush;
    Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> m_syncBorderBrush;
    Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> m_chevronBrush;
    Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> m_badgeBgBrush;
    Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> m_badgeTextBrush;
    Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> m_dividerBrush;
    Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> m_whiteBrush;
    bool m_brushesDarkTheme{false};
    tools3000::core::AccentColorRGB m_brushesAccent{0.23f, 0.51f, 0.96f};
    bool m_brushesValid{false};

    Microsoft::WRL::ComPtr<IDWriteFactory> m_dwriteFactory;
    Microsoft::WRL::ComPtr<IDWriteTextFormat> m_textFormat;
    Microsoft::WRL::ComPtr<IDWriteTextFormat> m_boldFormat;
    Microsoft::WRL::ComPtr<IDWriteTextFormat> m_badgeFormat;
    Microsoft::WRL::ComPtr<IDWriteTextFormat> m_searchTitleFormat;
    Microsoft::WRL::ComPtr<IDWriteTextFormat> m_searchPathFormat;
    Microsoft::WRL::ComPtr<IDWriteTextFormat> m_searchMetaFormat;

    HDC m_memDC{nullptr};
    HBITMAP m_memBitmap{nullptr};
    HBITMAP m_oldBitmap{nullptr};
    int m_renderedWidth{0};
    int m_renderedHeight{0};
    bool m_menuOpen{false};

    // ── 内嵌搜索浮层状态（仅在 UI 线程读写，跨线程结果由 m_searchResultMutex 同步）──
    bool m_searchOpen{false};
    bool m_searchOnlyFolders{false};
    bool m_searchServiceStarting{false};
    bool m_searchServiceUnavailable{false};
    bool m_searchSuppressNextChange{false};
    int m_searchPopupWidth{480};
    int m_searchPopupHeight{360};
    int m_searchSelectedIndex{0};
    int m_searchHoveredIndex{-1};
    int m_searchScrollTop{0};
    bool m_searchFilterBadgeHovered{false};
    D2D1_RECT_F m_searchFilterBadgeRect{};
    std::string m_searchCurrentQuery;
    std::vector<InlineSearchEntry> m_searchResults;
    Microsoft::WRL::ComPtr<ID2D1DCRenderTarget> m_searchRenderTarget;
    HFONT m_searchEditFont{nullptr};
    HBRUSH m_searchEditBgBrush{nullptr};
    COLORREF m_searchEditBgColor{RGB(30, 33, 42)};
    COLORREF m_searchEditTextColor{RGB(245, 247, 250)};
    bool m_searchEditDarkTheme{true};
    float m_searchEditFontDpi{0.0f};

    // 独立单线程受管搜索 Worker（杜绝与导航线程互相阻塞，0 detach）
    std::mutex m_searchWorkerMutex;
    std::condition_variable m_searchWorkerCv;
    std::thread m_searchWorker;
    std::optional<SearchWorkerJob> m_pendingSearchJob;
    std::atomic<bool> m_searchWorkerRunning{false};
    std::atomic<uint64_t> m_searchQuerySeq{0};
    std::atomic<bool> m_searchCancelFlag{false};

    std::mutex m_searchResultMutex;
    std::optional<PendingSearchResult> m_pendingSearchResult;

    std::vector<RibbonButtonRect> m_buttons;
    bool m_trackingMouse{false};
    mutable std::mutex m_mutex;
};

} // namespace tools3000::dialog
