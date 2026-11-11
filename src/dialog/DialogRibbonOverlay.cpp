/**
 * Tools3000 - High Performance Windows Productivity Suite
 * 
 * Copyright (c) 2026 Yy1 (GitHub yuan278501381) <https://github.com/yuan278501381> & Tools3000 contributors
 * 
 * Licensed under the MIT License.
 */

#include "DialogRibbonOverlay.h"
#include "DialogEventAndLayoutPolicy.h"
#include "PathMemoryManager.h"
#include "ExplorerTracker.h"
#include "DialogNavigator.h"
#include "core/logger/Logger.h"
#include "core/utils/WinUtils.h"
#include "core/utils/DpiUtils.h"
#include "core/utils/DWriteFontUtils.h"
#include "core/utils/ThemeUtils.h"
#include "core/events/MainThreadDispatcher.h"
#include "core/config/ConfigManager.h"
#include <algorithm>
#include <cmath>

#pragma comment(lib, "d2d1.lib")
#pragma comment(lib, "dwrite.lib")

using Microsoft::WRL::ComPtr;

namespace tools3000::dialog {

namespace {
const wchar_t* const RIBBON_WINDOW_CLASS = L"Tools3000_DialogRibbonOverlay";
constexpr UINT WM_RIBBON_UPDATE_POS    = WM_USER + 101;
constexpr UINT WM_RIBBON_ATTACH        = WM_USER + 102;
constexpr UINT WM_RIBBON_HIDE          = WM_USER + 103;
constexpr UINT WM_RIBBON_THEME_CHANGED = WM_USER + 104;

bool isRibbonEnglishLocale() {
    return !tools3000::core::isUiLanguageChinese();
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
}

DialogRibbonOverlay& DialogRibbonOverlay::instance() {
    static DialogRibbonOverlay s_instance;
    return s_instance;
}

DialogRibbonOverlay::DialogRibbonOverlay() = default;

DialogRibbonOverlay::~DialogRibbonOverlay() {
    cleanup();
}

bool DialogRibbonOverlay::init() {
    {
        std::lock_guard lock(m_mutex);
        if (m_hwnd && IsWindow(m_hwnd)) return true;
    }

    // 严禁在无 Win32 消息循环的后台工作线程中就地创建 HWND。
    // 若当前不在 UI 主线程且 MainThreadDispatcher 已初始化，必须调度回 UI 主线程建窗。
    if (tools3000::core::MainThreadDispatcher::instance().isInitialized() &&
        !tools3000::core::MainThreadDispatcher::instance().isOwnerThread()) {
        tools3000::core::MainThreadDispatcher::instance().post([this]() {
            init();
        });
        return false;
    }

    WNDCLASSEXW wc = {sizeof(WNDCLASSEXW)};
    wc.lpfnWndProc = WndProc;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpszClassName = RIBBON_WINDOW_CLASS;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    RegisterClassExW(&wc);

    D2D1CreateFactory(D2D1_FACTORY_TYPE_MULTI_THREADED, m_d2dFactory.GetAddressOf());
    DWriteCreateFactory(
        DWRITE_FACTORY_TYPE_SHARED,
        __uuidof(IDWriteFactory),
        reinterpret_cast<IUnknown**>(m_dwriteFactory.GetAddressOf())
    );
    ensureTextFormats(m_dpiScale);

    HWND createdHwnd = CreateWindowExW(
        WS_EX_LAYERED | WS_EX_TOOLWINDOW | WS_EX_TOPMOST | WS_EX_NOACTIVATE,
        RIBBON_WINDOW_CLASS,
        L"Tools3000 Dialog Ribbon",
        WS_POPUP,
        0, 0, m_width, m_height,
        nullptr, nullptr, GetModuleHandleW(nullptr), this
    );

    if (!createdHwnd) {
        LOG_ERROR("创建 DialogRibbonOverlay 窗口失败");
        return false;
    }

    {
        std::lock_guard lock(m_mutex);
        m_hwnd = createdHwnd;
    }

    HDC screenDC = GetDC(nullptr);
    m_memDC = CreateCompatibleDC(screenDC);
    ReleaseDC(nullptr, screenDC);

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

void DialogRibbonOverlay::cleanup() {
    if (tools3000::core::MainThreadDispatcher::instance().isInitialized() &&
        !tools3000::core::MainThreadDispatcher::instance().isOwnerThread()) {
        tools3000::core::MainThreadDispatcher::instance().post([this]() {
            cleanup();
        });
        return;
    }

    HWND hwndToDestroy = nullptr;
    {
        std::lock_guard lock(m_mutex);
        hwndToDestroy = m_hwnd;
        m_hwnd = nullptr;
        m_dismissedDialog = nullptr;
        resetStateLocked();
    }

    if (hwndToDestroy && IsWindow(hwndToDestroy)) {
        DestroyWindow(hwndToDestroy);
    }
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
    m_textFormat.Reset();
    m_boldFormat.Reset();
    m_badgeFormat.Reset();
    m_dwriteFactory.Reset();
    m_d2dFactory.Reset();

    // 触发冷路径物理内存修剪
    tools3000::core::WinUtils::trimWorkingSet();
}

void DialogRibbonOverlay::setCachedDialogFolder(HWND dialogHwnd, const std::string& folderPath) {
    if (!dialogHwnd) return;
    std::lock_guard lock(m_mutex);
    if (m_targetDialog == nullptr || m_targetDialog == dialogHwnd) {
        m_cachedDialogFolder = folderPath;
    }
}

void DialogRibbonOverlay::attachValidatedOnUiThread(HWND dialogHwnd,
                                                    DWORD processId,
                                                    const std::string& processName) {
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
        if (m_targetDialog != dialogHwnd) {
            m_cachedDialogFolder.clear();
        }
        m_targetDialog = dialogHwnd;
        m_targetProcessId = processId;
        m_targetProcess = processName;
    }

    // 主动请求后台刷新一次活动 Explorer 路径快照（0ms 非阻塞）
    ExplorerTracker::instance().requestRefresh();

    if (hwnd && IsWindow(hwnd)) {
        PostMessageW(hwnd, WM_RIBBON_ATTACH, 0, 0);
    }
}

void DialogRibbonOverlay::attachToDialog(HWND dialogHwnd, const std::string& processName) {
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
    if (disp.isInitialized() && disp.isOwnerThread()) {
        // 严禁在主 UI 线程直接调用跨进程 DialogNavigator::isFileDialog！
        // 投递至专用后台 MTA 工作线程完成结构验证与初始目录探测后再调度回主线程附着。
        DialogNavigator::instance().postAsyncTask([this, dialogHwnd, processId, processName]() {
            if (!IsWindow(dialogHwnd) || !DialogNavigator::isFileDialog(dialogHwnd)) {
                return;
            }
            const std::string folder = DialogNavigator::getCurrentDialogFolder(dialogHwnd);
            tools3000::core::MainThreadDispatcher::instance().post(
                [this, dialogHwnd, processId, processName, folder]() {
                    attachValidatedOnUiThread(dialogHwnd, processId, processName);
                    if (!folder.empty()) {
                        setCachedDialogFolder(dialogHwnd, folder);
                    }
                });
        });
        return;
    }

    // 当前已在后台线程：直接执行验证，避免主线程再次做跨进程探测
    if (!DialogNavigator::isFileDialog(dialogHwnd)) {
        return;
    }

    if (disp.isInitialized()) {
        disp.post([this, dialogHwnd, processId, processName]() {
            attachValidatedOnUiThread(dialogHwnd, processId, processName);
        });
        return;
    }

    attachValidatedOnUiThread(dialogHwnd, processId, processName);
}

void DialogRibbonOverlay::updatePosition() {
    const HWND hwnd = getHwnd();
    if (hwnd && IsWindow(hwnd)) {
        PostMessageW(hwnd, WM_RIBBON_UPDATE_POS, 0, 0);
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

void DialogRibbonOverlay::doAttachToDialog() {
    {
        std::lock_guard lock(m_mutex);
        m_buttons.clear();
        m_renderedWidth = 0;
        m_renderedHeight = 0;
    }
    // doUpdatePosition is the single authority that may show the overlay.
    // A queued stale ATTACH must never re-show it after validation failed.
    doUpdatePosition();
}

void DialogRibbonOverlay::doHide() {
    HWND hwnd = nullptr;
    {
        std::lock_guard lock(m_mutex);
        hwnd = m_hwnd;
        resetStateLocked();
    }
    // 严格在释放 m_mutex 之后再调用同步 Win32 ShowWindow，杜绝同步窗口消息重入死锁
    if (hwnd && IsWindow(hwnd)) {
        ShowWindow(hwnd, SW_HIDE);
    }
}

void DialogRibbonOverlay::resetStateLocked() {
    m_targetDialog = nullptr;
    m_targetProcessId = 0;
    m_targetProcess.clear();
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
    bool oldEnglishLocale = false;
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
        oldEnglishLocale = m_isEnglishLocale;
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
            // HWND values can be reused after a dialog is destroyed. PID plus
            // dialog class validation prevents following the replacement window.
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

        // 如果下拉菜单正在弹出交互中，始终保持展示
        if (menuOpen) {
            return true;
        }

        // 1. 检查目标对话框是否最小化或已被隐藏（全部在锁外调用 ShowWindow）
        if (IsIconic(targetDialog) || !IsWindowVisible(targetDialog)) {
            if (IsWindowVisible(hwnd)) ShowWindow(hwnd, SW_HIDE);
            return false;
        }

        // 2. 前台归属硬校验：胶囊只属于文件对话框本身。
        const HWND foreground = GetForegroundWindow();
        const HWND foregroundRoot = foreground ? GetAncestor(foreground, GA_ROOT) : nullptr;
        if (foreground != hwnd && foreground != targetDialog &&
            foregroundRoot != targetDialog) {
            if (IsWindowVisible(hwnd)) ShowWindow(hwnd, SW_HIDE);
            return false;
        }

        RECT dlgRect{};
        if (!GetWindowRect(targetDialog, &dlgRect)) {
            doHide();
            return false;
        }

        const float newDpiScale =
            std::clamp(tools3000::core::dpi::scaleForWindow(targetDialog), 0.75f, 4.0f);
        const bool dpiChanged = std::abs(newDpiScale - oldDpiScale) > 0.001f;
        const bool langChanged = (isRibbonEnglishLocale() != oldEnglishLocale);

        int probePosX = 0;
        int maxAvailableWidth = 0;
        const std::string posConfig = PathMemoryManager::instance().getRibbonPosition();
        computeSafeHorizontalPlacement(dlgRect, newDpiScale, 64, posConfig, probePosX, maxAvailableWidth);
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
                !computeSafeHorizontalPlacement(dlgRect, newDpiScale, m_width, posConfig, posX, maxAvailableWidth)) {
                winWidth = 0;
            } else {
                posY = dlgRect.top + static_cast<int>(std::round(5.0f * newDpiScale));
                m_posX = posX;
                m_posY = posY;
                winWidth = m_width;
                winHeight = m_height;
                needRender = layoutRebuilt || m_renderedWidth != m_width || m_renderedHeight != m_height;
            }
        }

        // 严格在释放 m_mutex 之后再调用同步 SetWindowPos / render / ShowWindow
        if (winWidth <= 0 || winHeight <= 0) {
            if (IsWindowVisible(hwnd)) ShowWindow(hwnd, SW_HIDE);
            return false;
        }

        SetWindowPos(hwnd, HWND_TOPMOST, posX, posY, winWidth, winHeight, SWP_NOACTIVATE);
        if (needRender) {
            render();
        }
        if (!IsWindowVisible(hwnd)) {
            ShowWindow(hwnd, SW_SHOWNOACTIVATE);
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
    const bool isEn = isRibbonEnglishLocale();
    if (m_dwriteFactory &&
        (m_textFormat == nullptr || std::abs(m_currentFontDpi - dpiScale) > 0.001f || m_isEnglishLocale != isEn)) {
        m_textFormat.Reset();
        m_boldFormat.Reset();
        m_badgeFormat.Reset();
        m_currentFontDpi = dpiScale;
        m_isEnglishLocale = isEn;
        const wchar_t* localeName = isEn ? L"en-US" : L"zh-CN";
        const wchar_t* fontName = tools3000::core::resolveInstalledUiFontFamily(m_dwriteFactory.Get());

        const float fontSize = 12.0f * dpiScale;
        const float badgeSize = 12.0f * dpiScale;

        HRESULT hr = m_dwriteFactory->CreateTextFormat(
            fontName,
            nullptr,
            DWRITE_FONT_WEIGHT_MEDIUM,
            DWRITE_FONT_STYLE_NORMAL,
            DWRITE_FONT_STRETCH_NORMAL,
            fontSize,
            localeName,
            m_textFormat.GetAddressOf()
        );
        if (SUCCEEDED(hr) && m_textFormat) {
            m_dwriteFactory->CreateTextFormat(
                fontName,
                nullptr,
                DWRITE_FONT_WEIGHT_SEMI_BOLD,
                DWRITE_FONT_STYLE_NORMAL,
                DWRITE_FONT_STRETCH_NORMAL,
                fontSize,
                localeName,
                m_boldFormat.GetAddressOf()
            );
            m_dwriteFactory->CreateTextFormat(
                fontName,
                nullptr,
                DWRITE_FONT_WEIGHT_BOLD,
                DWRITE_FONT_STYLE_NORMAL,
                DWRITE_FONT_STRETCH_NORMAL,
                badgeSize,
                localeName,
                m_badgeFormat.GetAddressOf()
            );
            configureSingleLineTrimming(m_dwriteFactory.Get(), m_textFormat.Get());
            configureSingleLineTrimming(m_dwriteFactory.Get(), m_boldFormat.Get());
            configureSingleLineTrimming(m_dwriteFactory.Get(), m_badgeFormat.Get());
        }
    }
}

void DialogRibbonOverlay::drawFolderIcon(ID2D1RenderTarget* rt, ID2D1Brush* brush, float x, float y, float size, float strokeWidth) {
    if (!rt || !brush) return;
    float tabW = size * 0.42f;
    float tabH = size * 0.28f;
    float corner = size * 0.15f;

    // 绘制极简文件夹轮廓
    D2D1_RECT_F bodyRect = D2D1::RectF(x, y + tabH * 0.7f, x + size, y + size);
    rt->DrawRoundedRectangle(D2D1::RoundedRect(bodyRect, corner, corner), brush, strokeWidth);

    // 顶部折角标签
    rt->DrawLine(D2D1::Point2F(x + corner, y + tabH * 0.7f), D2D1::Point2F(x + corner, y + corner), brush, strokeWidth);
    rt->DrawLine(D2D1::Point2F(x + corner, y + corner), D2D1::Point2F(x + tabW, y + corner), brush, strokeWidth);
    rt->DrawLine(D2D1::Point2F(x + tabW, y + corner), D2D1::Point2F(x + tabW + tabH * 0.5f, y + tabH * 0.7f), brush, strokeWidth);
}

void DialogRibbonOverlay::drawClockIcon(ID2D1RenderTarget* rt, ID2D1Brush* brush, float x, float y, float size, float strokeWidth) {
    if (!rt || !brush) return;
    float cx = x + size * 0.5f;
    float cy = y + size * 0.5f;
    float r = size * 0.48f;

    rt->DrawEllipse(D2D1::Ellipse(D2D1::Point2F(cx, cy), r, r), brush, strokeWidth);
    // 时针与分针
    rt->DrawLine(D2D1::Point2F(cx, cy), D2D1::Point2F(cx, cy - r * 0.58f), brush, strokeWidth);
    rt->DrawLine(D2D1::Point2F(cx, cy), D2D1::Point2F(cx + r * 0.45f, cy), brush, strokeWidth);
}

void DialogRibbonOverlay::drawBookmarkIcon(ID2D1RenderTarget* rt, ID2D1Brush* brush, float x, float y, float size, float strokeWidth) {
    if (!rt || !brush) return;
    float w = size * 0.75f;
    float h = size;
    float left = x + (size - w) * 0.5f;
    float right = left + w;
    float bottom = y + h;
    float notchY = bottom - h * 0.28f;
    float midX = left + w * 0.5f;

    rt->DrawLine(D2D1::Point2F(left, y), D2D1::Point2F(right, y), brush, strokeWidth);
    rt->DrawLine(D2D1::Point2F(right, y), D2D1::Point2F(right, bottom), brush, strokeWidth);
    rt->DrawLine(D2D1::Point2F(right, bottom), D2D1::Point2F(midX, notchY), brush, strokeWidth);
    rt->DrawLine(D2D1::Point2F(midX, notchY), D2D1::Point2F(left, bottom), brush, strokeWidth);
    rt->DrawLine(D2D1::Point2F(left, bottom), D2D1::Point2F(left, y), brush, strokeWidth);
}

void DialogRibbonOverlay::drawChevron(ID2D1RenderTarget* rt, ID2D1Brush* brush, float centerX, float centerY, float size, float strokeWidth) {
    if (!rt || !brush) return;
    float halfW = size * 0.5f;
    float halfH = size * 0.28f;
    rt->DrawLine(
        D2D1::Point2F(centerX - halfW, centerY - halfH),
        D2D1::Point2F(centerX, centerY + halfH),
        brush, strokeWidth, nullptr
    );
    rt->DrawLine(
        D2D1::Point2F(centerX, centerY + halfH),
        D2D1::Point2F(centerX + halfW, centerY - halfH),
        brush, strokeWidth, nullptr
    );
}

void DialogRibbonOverlay::drawCloseIcon(ID2D1RenderTarget* rt, ID2D1Brush* brush, float centerX, float centerY, float size, float strokeWidth) {
    if (!rt || !brush) return;
    const float half = size * 0.5f;
    rt->DrawLine(
        D2D1::Point2F(centerX - half, centerY - half),
        D2D1::Point2F(centerX + half, centerY + half),
        brush, strokeWidth, nullptr
    );
    rt->DrawLine(
        D2D1::Point2F(centerX + half, centerY - half),
        D2D1::Point2F(centerX - half, centerY + half),
        brush, strokeWidth, nullptr
    );
}

void DialogRibbonOverlay::drawBadge(ID2D1RenderTarget* rt, ID2D1Brush* bgBrush, ID2D1Brush* textBrush, float x, float y, float w, float h, int count) {
    if (!rt || !bgBrush || !textBrush || !m_badgeFormat) return;
    D2D1_RECT_F badgeRect = D2D1::RectF(x, y, x + w, y + h);
    float radius = h * 0.5f;
    rt->FillRoundedRectangle(D2D1::RoundedRect(badgeRect, radius, radius), bgBrush);

    std::wstring countStr = std::to_wstring(count);
    m_badgeFormat->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
    m_badgeFormat->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
    rt->DrawText(
        countStr.c_str(), static_cast<UINT32>(countStr.length()),
        m_badgeFormat.Get(), badgeRect, textBrush
    );
}

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
        HRESULT hr = m_dwriteFactory->CreateTextLayout(
            wText.c_str(), static_cast<UINT32>(wText.length()),
            format, 2000.0f, 100.0f, layout.GetAddressOf()
        );
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

    m_height = static_cast<int>(std::round(32.0f * safeScale));
    const float paddingX = 6.0f * safeScale;
    const float btnH = 24.0f * safeScale;
    const float btnY = (static_cast<float>(m_height) - btnH) * 0.5f;
    const float syncDividerGap = 10.0f * safeScale;
    const float interButtonGap = 4.0f * safeScale;
    const float minSyncWidth = 68.0f * safeScale;
    const float closeW = 22.0f * safeScale;

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
    const std::string recentLabel = m_isEnglishLocale ? "Recent" : "最近";
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

    const std::string favLabel = m_isEnglishLocale ? "Workspaces" : "工作区";
    const float favTextW = measureText(favLabel, false) + 2.0f * safeScale;
    const float favW = favTextW + 38.0f * safeScale;
    bool hasFav = true;
    bool hasClose = true;

    float syncFinalW = syncNaturalW;
    auto calcTotalWidth = [&]() -> float {
        float w = paddingX;
        int primaryCount = 0;
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
        }
    }

    float currentX = paddingX;
    if (hasSync) {
        m_buttons.push_back({
            RibbonButtonRect::Type::ExplorerSync,
            D2D1::RectF(currentX, btnY, currentX + syncFinalW, btnY + btnH),
            syncLabel,
            m_activeExplorerPath,
            0,
            false,
            (std::max)(0.0f, syncFinalW - 33.0f * safeScale),
            0.0f
        });
        currentX += syncFinalW + ((hasRecent || hasFav || hasClose) ? syncDividerGap : 0.0f);
    }

    if (hasRecent) {
        m_buttons.push_back({
            RibbonButtonRect::Type::Recent,
            D2D1::RectF(currentX, btnY, currentX + recentW, btnY + btnH),
            recentLabel,
            "",
            recentCount,
            false,
            recentTextW,
            recentBadgeW
        });
        currentX += recentW + ((hasFav || hasClose) ? interButtonGap : 0.0f);
    }

    if (hasFav) {
        m_buttons.push_back({
            RibbonButtonRect::Type::Favorites,
            D2D1::RectF(currentX, btnY, currentX + favW, btnY + btnH),
            favLabel,
            "",
            0,
            false,
            favTextW,
            0.0f
        });
        currentX += favW + (hasClose ? interButtonGap : 0.0f);
    }

    if (hasClose && !m_buttons.empty()) {
        m_buttons.push_back({
            RibbonButtonRect::Type::Close,
            D2D1::RectF(currentX, btnY, currentX + closeW, btnY + btnH),
            "",
            "",
            0,
            false,
            0.0f,
            0.0f
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
        m_renderTarget->CreateSolidColorBrush(D2D1::ColorF(0.12f, 0.13f, 0.17f, 0.93f), m_bgBrush.GetAddressOf());
        m_renderTarget->CreateSolidColorBrush(D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.13f), m_borderBrush.GetAddressOf());
        m_renderTarget->CreateSolidColorBrush(D2D1::ColorF(0.96f, 0.97f, 0.98f, 0.96f), m_textBrush.GetAddressOf());
        m_renderTarget->CreateSolidColorBrush(D2D1::ColorF(0.72f, 0.76f, 0.82f, 0.88f), m_textMutedBrush.GetAddressOf());
        m_renderTarget->CreateSolidColorBrush(D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.09f), m_highlightBrush.GetAddressOf());
        m_renderTarget->CreateSolidColorBrush(D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.15f), m_highlightBorderBrush.GetAddressOf());
        m_renderTarget->CreateSolidColorBrush(D2D1::ColorF(accent.r, accent.g, accent.b, 0.95f), m_syncBgBrush.GetAddressOf());
        m_renderTarget->CreateSolidColorBrush(D2D1::ColorF(borderR, borderG, borderB, 0.45f), m_syncBorderBrush.GetAddressOf());
        m_renderTarget->CreateSolidColorBrush(D2D1::ColorF(0.70f, 0.74f, 0.82f, 0.75f), m_chevronBrush.GetAddressOf());
        m_renderTarget->CreateSolidColorBrush(D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.12f), m_badgeBgBrush.GetAddressOf());
        m_renderTarget->CreateSolidColorBrush(D2D1::ColorF(0.95f, 0.95f, 0.98f, 0.95f), m_badgeTextBrush.GetAddressOf());
        m_renderTarget->CreateSolidColorBrush(D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.12f), m_dividerBrush.GetAddressOf());
    } else {
        m_renderTarget->CreateSolidColorBrush(D2D1::ColorF(0.965f, 0.975f, 0.985f, 0.96f), m_bgBrush.GetAddressOf());
        m_renderTarget->CreateSolidColorBrush(D2D1::ColorF(0.0f, 0.0f, 0.0f, 0.09f), m_borderBrush.GetAddressOf());
        m_renderTarget->CreateSolidColorBrush(D2D1::ColorF(0.10f, 0.12f, 0.16f, 0.95f), m_textBrush.GetAddressOf());
        m_renderTarget->CreateSolidColorBrush(D2D1::ColorF(0.32f, 0.36f, 0.44f, 0.88f), m_textMutedBrush.GetAddressOf());
        m_renderTarget->CreateSolidColorBrush(D2D1::ColorF(0.0f, 0.0f, 0.0f, 0.05f), m_highlightBrush.GetAddressOf());
        m_renderTarget->CreateSolidColorBrush(D2D1::ColorF(0.0f, 0.0f, 0.0f, 0.08f), m_highlightBorderBrush.GetAddressOf());
        m_renderTarget->CreateSolidColorBrush(D2D1::ColorF(accent.r * 0.94f, accent.g * 0.94f, accent.b * 0.94f, 0.95f), m_syncBgBrush.GetAddressOf());
        m_renderTarget->CreateSolidColorBrush(D2D1::ColorF(borderR, borderG, borderB, 0.50f), m_syncBorderBrush.GetAddressOf());
        m_renderTarget->CreateSolidColorBrush(D2D1::ColorF(0.40f, 0.44f, 0.52f, 0.75f), m_chevronBrush.GetAddressOf());
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

    {
        std::lock_guard lock(m_mutex);
        hwnd = m_hwnd;
        width = m_width;
        height = m_height;
        posX = m_posX;
        posY = m_posY;
        dpiScale = m_dpiScale;
        buttons = m_buttons;
    }

    if (!hwnd || !m_memDC || width <= 0 || height <= 0) return;

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
        BITMAPINFO bmi = {};
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
            D2D1_RENDER_TARGET_USAGE_GDI_COMPATIBLE
        );
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

    // 绘制主体全圆角微晶磨砂胶囊 (Full Pill Capsule)，内缩半线宽防 DIB 四边切平
    const float inset = 0.5f * dpiScale;
    const float capsuleRadius = (std::max)(0.0f, (static_cast<float>(height) * 0.5f) - inset);
    D2D1_ROUNDED_RECT mainRRect = D2D1::RoundedRect(
        D2D1::RectF(inset, inset, static_cast<float>(width) - inset, static_cast<float>(height) - inset),
        capsuleRadius, capsuleRadius
    );

    if (m_bgBrush) m_renderTarget->FillRoundedRectangle(&mainRRect, m_bgBrush.Get());
    if (m_borderBrush) m_renderTarget->DrawRoundedRectangle(&mainRRect, m_borderBrush.Get(), 1.0f * dpiScale);

    // 绘制各个按钮与微交互
    for (size_t i = 0; i < buttons.size(); ++i) {
        const auto& btn = buttons[i];
        float btnRadius = (btn.rect.bottom - btn.rect.top) * 0.5f;
        D2D1_ROUNDED_RECT btnRRect = D2D1::RoundedRect(btn.rect, btnRadius, btnRadius);

        if (btn.type == RibbonButtonRect::Type::ExplorerSync && m_syncBgBrush) {
            // 1. 同步路径主胶囊
            m_syncBgBrush->SetOpacity(btn.hovered ? 1.0f : 0.92f);
            m_renderTarget->FillRoundedRectangle(&btnRRect, m_syncBgBrush.Get());
            if (m_syncBorderBrush) {
                m_renderTarget->DrawRoundedRectangle(&btnRRect, m_syncBorderBrush.Get(), 1.0f * dpiScale);
            }

            // 矢量文件夹图标 (纯白)
            float iconSize = 12.0f * dpiScale;
            float iconX = btn.rect.left + 8.0f * dpiScale;
            float iconY = (btn.rect.top + btn.rect.bottom - iconSize) * 0.5f;
            drawFolderIcon(m_renderTarget.Get(), m_whiteBrush.Get(), iconX, iconY, iconSize, 1.2f * dpiScale);

            // 单行省略号截断渲染目录名
            std::wstring wText = tools3000::core::WinUtils::utf8ToWstring(btn.text);
            if (m_whiteBrush && m_boldFormat) {
                D2D1_RECT_F textRect = D2D1::RectF(
                    iconX + iconSize + 5.0f * dpiScale, btn.rect.top,
                    btn.rect.right - 8.0f * dpiScale, btn.rect.bottom
                );
                m_boldFormat->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
                m_boldFormat->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
                m_renderTarget->DrawText(
                    wText.c_str(), static_cast<UINT32>(wText.length()),
                    m_boldFormat.Get(), textRect, m_whiteBrush.Get()
                );
            }

            // 在同步胶囊右侧绘制垂直微晶分割线（仅当右侧还有其他胶囊时绘制）
            if (m_dividerBrush && (i + 1 < buttons.size())) {
                float divX = btn.rect.right + 5.0f * dpiScale;
                float divY1 = (static_cast<float>(height) - 14.0f * dpiScale) * 0.5f;
                float divY2 = divY1 + 14.0f * dpiScale;
                m_renderTarget->DrawLine(
                    D2D1::Point2F(divX, divY1),
                    D2D1::Point2F(divX, divY2),
                    m_dividerBrush.Get(), 1.0f * dpiScale
                );
            }
        } else if (btn.type == RibbonButtonRect::Type::Recent) {
            // 2. 最近使用项 (时钟图标 + 文字 + 独立数字徽章 + Chevron)
            if (btn.hovered && m_highlightBrush) {
                m_renderTarget->FillRoundedRectangle(&btnRRect, m_highlightBrush.Get());
                if (m_highlightBorderBrush) {
                    m_renderTarget->DrawRoundedRectangle(&btnRRect, m_highlightBorderBrush.Get(), 1.0f * dpiScale);
                }
            }

            auto activeTextBrush = btn.hovered ? m_textBrush.Get() : m_textMutedBrush.Get();
            auto activeChevronBrush = btn.hovered ? m_textBrush.Get() : m_chevronBrush.Get();

            float iconSize = 11.5f * dpiScale;
            float iconX = btn.rect.left + 7.0f * dpiScale;
            float iconY = (btn.rect.top + btn.rect.bottom - iconSize) * 0.5f;
            drawClockIcon(m_renderTarget.Get(), activeTextBrush, iconX, iconY, iconSize, 1.15f * dpiScale);

            std::wstring wText = tools3000::core::WinUtils::utf8ToWstring(btn.text);
            float textLeft = iconX + iconSize + 4.5f * dpiScale;
            float textW = btn.textWidth > 0.0f ? btn.textWidth : (36.0f * dpiScale);
            if (activeTextBrush && m_textFormat) {
                D2D1_RECT_F textRect = D2D1::RectF(textLeft, btn.rect.top, textLeft + textW, btn.rect.bottom);
                m_textFormat->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
                m_textFormat->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
                m_renderTarget->DrawText(
                    wText.c_str(), static_cast<UINT32>(wText.length()),
                    m_textFormat.Get(), textRect, activeTextBrush
                );
            }

            float badgeW = btn.badgeWidth > 0.0f ? btn.badgeWidth : (16.0f * dpiScale);
            float badgeH = 14.0f * dpiScale;
            float badgeX = textLeft + textW + 3.0f * dpiScale;
            float badgeY = (btn.rect.top + btn.rect.bottom - badgeH) * 0.5f;
            drawBadge(m_renderTarget.Get(), m_badgeBgBrush.Get(), m_badgeTextBrush.Get(), badgeX, badgeY, badgeW, badgeH, btn.badgeCount);

            float chevronCenterX = btn.rect.right - 7.5f * dpiScale;
            float chevronCenterY = (btn.rect.top + btn.rect.bottom) * 0.5f;
            drawChevron(
                m_renderTarget.Get(), activeChevronBrush,
                chevronCenterX, chevronCenterY,
                5.5f * dpiScale, 1.2f * dpiScale
            );
        } else if (btn.type == RibbonButtonRect::Type::Favorites) {
            // 3. 工作区收藏项 (书签图标 + 文字 + Chevron)
            if (btn.hovered && m_highlightBrush) {
                m_renderTarget->FillRoundedRectangle(&btnRRect, m_highlightBrush.Get());
                if (m_highlightBorderBrush) {
                    m_renderTarget->DrawRoundedRectangle(&btnRRect, m_highlightBorderBrush.Get(), 1.0f * dpiScale);
                }
            }

            auto activeTextBrush = btn.hovered ? m_textBrush.Get() : m_textMutedBrush.Get();
            auto activeChevronBrush = btn.hovered ? m_textBrush.Get() : m_chevronBrush.Get();

            float iconSize = 11.0f * dpiScale;
            float iconX = btn.rect.left + 7.0f * dpiScale;
            float iconY = (btn.rect.top + btn.rect.bottom - iconSize) * 0.5f;
            drawBookmarkIcon(m_renderTarget.Get(), activeTextBrush, iconX, iconY, iconSize, 1.15f * dpiScale);

            std::wstring wText = tools3000::core::WinUtils::utf8ToWstring(btn.text);
            float textLeft = iconX + iconSize + 4.5f * dpiScale;
            float textW = btn.textWidth > 0.0f ? btn.textWidth : (48.0f * dpiScale);
            if (activeTextBrush && m_textFormat) {
                D2D1_RECT_F textRect = D2D1::RectF(textLeft, btn.rect.top, textLeft + textW, btn.rect.bottom);
                m_textFormat->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
                m_textFormat->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
                m_renderTarget->DrawText(
                    wText.c_str(), static_cast<UINT32>(wText.length()),
                    m_textFormat.Get(), textRect, activeTextBrush
                );
            }

            float chevronCenterX = btn.rect.right - 7.5f * dpiScale;
            float chevronCenterY = (btn.rect.top + btn.rect.bottom) * 0.5f;
            drawChevron(
                m_renderTarget.Get(), activeChevronBrush,
                chevronCenterX, chevronCenterY,
                5.5f * dpiScale, 1.2f * dpiScale
            );
        } else if (btn.type == RibbonButtonRect::Type::Close) {
            // 4. 关闭胶囊按钮 [X]（纯矢量交叉线，尺寸与命中热区均严格乘 m_dpiScale）
            if (btn.hovered && m_highlightBrush) {
                m_renderTarget->FillRoundedRectangle(&btnRRect, m_highlightBrush.Get());
                if (m_highlightBorderBrush) {
                    m_renderTarget->DrawRoundedRectangle(&btnRRect, m_highlightBorderBrush.Get(), 1.0f * dpiScale);
                }
            }
            auto closeIconBrush = btn.hovered ? m_textBrush.Get() : m_chevronBrush.Get();
            const float cx = (btn.rect.left + btn.rect.right) * 0.5f;
            const float cy = (btn.rect.top + btn.rect.bottom) * 0.5f;
            drawCloseIcon(m_renderTarget.Get(), closeIconBrush, cx, cy, 7.0f * dpiScale, 1.25f * dpiScale);
        }
    }

    const HRESULT endHr = m_renderTarget->EndDraw();
    if (endHr == D2DERR_RECREATE_TARGET || FAILED(endHr)) {
        LOG_WARN("DialogRibbonOverlay: EndDraw returned 0x{:X}, resetting D2D target and brushes for auto-recovery",
                 static_cast<unsigned>(endHr));
        discardDeviceResources();
        {
            std::lock_guard lock(m_mutex);
            m_renderedWidth = 0;
            m_renderedHeight = 0;
        }
        if (IsWindow(hwnd)) {
            PostMessageW(hwnd, WM_RIBBON_UPDATE_POS, 0, 0);
        }
        return;
    }

    {
        std::lock_guard lock(m_mutex);
        m_renderedWidth = width;
        m_renderedHeight = height;
    }

    // 提交分层窗口
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
    const float padX = (btn.type == RibbonButtonRect::Type::Close) ? (2.0f * safeScale) : 0.0f;
    const float padY = 2.0f * safeScale;
    const float fx = static_cast<float>(x);
    const float fy = static_cast<float>(y);
    return fx >= (btn.rect.left - padX) && fx <= (btn.rect.right + padX) &&
           fy >= (btn.rect.top - padY) && fy <= (btn.rect.bottom + padY);
}

void DialogRibbonOverlay::onMouseMove(int x, int y) {
    HWND hwnd = getHwnd();
    if (!m_trackingMouse && hwnd && IsWindow(hwnd)) {
        TRACKMOUSEEVENT tme = {sizeof(TRACKMOUSEEVENT), TME_LEAVE, hwnd, 0};
        TrackMouseEvent(&tme);
        m_trackingMouse = true;
    }

    bool needRepaint = false;
    {
        std::lock_guard lock(m_mutex);
        for (auto& btn : m_buttons) {
            const bool inside = hitTestButton(btn, x, y, m_dpiScale);
            if (btn.hovered != inside) {
                btn.hovered = inside;
                needRepaint = true;
            }
        }
    }

    if (needRepaint) {
        render();
    }
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

    const std::string posConfig = PathMemoryManager::instance().getRibbonPosition();
    const bool rightAlign = (posConfig != "top-center") ||
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
    // 1. 弹出模态菜单前完整拷贝路径列表与目标快照到栈上局部变量，
    //    彻底免疫 TrackPopupMenu 消息循环期间异步 doHide() 清空成员变量导致的越界崩溃
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
    const int cmd = TrackPopupMenu(
        hMenu, alignFlags,
        screenX, screenY, 0, localHwnd, nullptr
    );
    DestroyMenu(hMenu);

    {
        std::lock_guard lock(m_mutex);
        m_menuOpen = false;
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
    // 1. 栈上局部快照：零跨进程 UIA 调用，零模态重入越界风险
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

    const bool isEn = isRibbonEnglishLocale();
    tools3000::core::WinUtils::applyNativeMenuTheme();
    HMENU hMenu = CreatePopupMenu();

    const bool canOperateOnDialog = (localTargetDialog && IsWindow(localTargetDialog));
    if (canOperateOnDialog) {
        const std::wstring folderSuffix = localCurFolder.empty()
            ? std::wstring{}
            : (L" (" + tools3000::core::WinUtils::utf8ToWstring(localCurFolder) + L")");
        const std::wstring addFavText = isEn
            ? (L"[+] Add Current Folder to Workspaces" + folderSuffix)
            : (L"[+] 将当前目录加入常用工作区" + folderSuffix);
        AppendMenuW(hMenu, MF_STRING, 2901, addFavText.c_str());

        if (!localTargetProcess.empty()) {
            const std::wstring wProc = tools3000::core::WinUtils::utf8ToWstring(localTargetProcess);
            const std::wstring lockText = isEn
                ? (L"[*] Pin Folder for Current App (" + wProc + L")")
                : (L"[*] 为当前软件 (" + wProc + L") 固定此目录");
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
    const int cmd = TrackPopupMenu(
        hMenu, alignFlags,
        screenX, screenY, 0, localHwnd, nullptr
    );
    DestroyMenu(hMenu);

    {
        std::lock_guard lock(m_mutex);
        m_menuOpen = false;
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

LRESULT CALLBACK DialogRibbonOverlay::WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    DialogRibbonOverlay* self = nullptr;
    if (msg == WM_CREATE) {
        auto* cs = reinterpret_cast<CREATESTRUCT*>(lParam);
        self = reinterpret_cast<DialogRibbonOverlay*>(cs->lpCreateParams);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
    } else {
        self = reinterpret_cast<DialogRibbonOverlay*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    }

    if (self) {
        switch (msg) {
            case WM_RIBBON_UPDATE_POS:
            case WM_TIMER:
            case WM_DPICHANGED:
                self->doUpdatePosition();
                return 0;
            case WM_RIBBON_ATTACH:
                self->doAttachToDialog();
                return 0;
            case WM_RIBBON_THEME_CHANGED: {
                self->m_brushesValid = false;
                bool hasTarget = false;
                {
                    std::lock_guard lock(self->m_mutex);
                    self->m_renderedWidth = 0;
                    self->m_renderedHeight = 0;
                    hasTarget = (self->m_targetDialog != nullptr);
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
        }
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

} // namespace tools3000::dialog
