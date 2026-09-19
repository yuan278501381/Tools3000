/**
 * Tools3000 - High Performance Windows Productivity Suite
 * 
 * Copyright (c) 2026 Yy1 (GitHub yuan278501381) <https://github.com/yuan278501381> & Tools3000 contributors
 * 
 * Licensed under the MIT License.
 */

#include "DialogRibbonOverlay.h"
#include "PathMemoryManager.h"
#include "ExplorerTracker.h"
#include "DialogNavigator.h"
#include "core/logger/Logger.h"
#include "core/utils/WinUtils.h"
#include "core/utils/DpiUtils.h"
#include "core/utils/ThemeUtils.h"
#include "core/events/MainThreadDispatcher.h"
#include "core/config/ConfigManager.h"
#include <algorithm>

#pragma comment(lib, "d2d1.lib")
#pragma comment(lib, "dwrite.lib")

using Microsoft::WRL::ComPtr;

namespace tools3000::dialog {

namespace {
const wchar_t* const RIBBON_WINDOW_CLASS = L"Tools3000_DialogRibbonOverlay";
constexpr UINT WM_RIBBON_UPDATE_POS = WM_USER + 101;
constexpr UINT WM_RIBBON_ATTACH     = WM_USER + 102;
constexpr UINT WM_RIBBON_HIDE       = WM_USER + 103;
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
    if (m_hwnd && IsWindow(m_hwnd)) return true;

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

    if (m_dwriteFactory) {
        m_dwriteFactory->CreateTextFormat(
            L"Microsoft YaHei UI",
            nullptr,
            DWRITE_FONT_WEIGHT_NORMAL,
            DWRITE_FONT_STYLE_NORMAL,
            DWRITE_FONT_STRETCH_NORMAL,
            12.0f,
            L"zh-CN",
            m_textFormat.GetAddressOf()
        );
        m_dwriteFactory->CreateTextFormat(
            L"Microsoft YaHei UI",
            nullptr,
            DWRITE_FONT_WEIGHT_SEMI_BOLD,
            DWRITE_FONT_STYLE_NORMAL,
            DWRITE_FONT_STRETCH_NORMAL,
            12.0f,
            L"zh-CN",
            m_boldFormat.GetAddressOf()
        );
    }

    m_hwnd = CreateWindowExW(
        WS_EX_LAYERED | WS_EX_TOOLWINDOW | WS_EX_TOPMOST | WS_EX_NOACTIVATE,
        RIBBON_WINDOW_CLASS,
        L"Tools3000 Dialog Ribbon",
        WS_POPUP,
        0, 0, m_width, m_height,
        nullptr, nullptr, GetModuleHandleW(nullptr), this
    );

    if (!m_hwnd) {
        LOG_ERROR("创建 DialogRibbonOverlay 窗口失败");
        return false;
    }

    HDC screenDC = GetDC(nullptr);
    m_memDC = CreateCompatibleDC(screenDC);
    ReleaseDC(nullptr, screenDC);

    return true;
}

void DialogRibbonOverlay::cleanup() {
    if (tools3000::core::MainThreadDispatcher::instance().isInitialized() &&
        !tools3000::core::MainThreadDispatcher::instance().isOwnerThread()) {
        tools3000::core::MainThreadDispatcher::instance().post([this]() {
            cleanup();
        });
        return;
    }
    if (m_hwnd && IsWindow(m_hwnd)) {
        DestroyWindow(m_hwnd);
        m_hwnd = nullptr;
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
    m_renderTarget.Reset();
    m_textFormat.Reset();
    m_boldFormat.Reset();
    m_badgeFormat.Reset();
    m_dwriteFactory.Reset();
    m_d2dFactory.Reset();

    // 触发冷路径物理内存修剪
    tools3000::core::WinUtils::trimWorkingSet();
}

void DialogRibbonOverlay::attachToDialog(HWND dialogHwnd, const std::string& processName) {
    if (!dialogHwnd || !IsWindow(dialogHwnd)) return;
    if (!PathMemoryManager::instance().isRibbonEnabled()) return;

    wchar_t className[64]{};
    if (GetClassNameW(dialogHwnd, className, static_cast<int>(std::size(className))) <= 0 ||
        wcscmp(className, L"#32770") != 0) {
        return;
    }

    // 防御门禁：必须是有效的文件/文件夹对话框，严禁附着于文件传输进度条或属性页
    if (!DialogNavigator::isFileDialog(dialogHwnd)) {
        return;
    }

    DWORD processId = 0;
    GetWindowThreadProcessId(dialogHwnd, &processId);
    if (processId == 0) return;

    if (tools3000::core::MainThreadDispatcher::instance().isInitialized() &&
        !tools3000::core::MainThreadDispatcher::instance().isOwnerThread()) {
        tools3000::core::MainThreadDispatcher::instance().post([this, dialogHwnd, processName]() {
            attachToDialog(dialogHwnd, processName);
        });
        return;
    }

    if (!m_hwnd || !IsWindow(m_hwnd)) {
        if (!init()) return;
    }

    {
        std::lock_guard lock(m_mutex);
        m_targetDialog = dialogHwnd;
        m_targetProcessId = processId;
        m_targetProcess = processName;
    }

    if (m_hwnd && IsWindow(m_hwnd)) {
        PostMessageW(m_hwnd, WM_RIBBON_ATTACH, 0, 0);
    }
}

void DialogRibbonOverlay::updatePosition() {
    if (m_hwnd && IsWindow(m_hwnd)) {
        PostMessageW(m_hwnd, WM_RIBBON_UPDATE_POS, 0, 0);
    }
}

void DialogRibbonOverlay::hide() {
    if (m_hwnd && IsWindow(m_hwnd)) {
        PostMessageW(m_hwnd, WM_RIBBON_HIDE, 0, 0);
    }
}

void DialogRibbonOverlay::doAttachToDialog() {
    // doUpdatePosition is the single authority that may show the overlay.
    // A queued stale ATTACH must never re-show it after validation failed.
    doUpdatePosition();
}

void DialogRibbonOverlay::doHide() {
    std::lock_guard lock(m_mutex);
    doHideLocked();
}

void DialogRibbonOverlay::doHideLocked() {
    if (m_hwnd && IsWindow(m_hwnd)) {
        ShowWindow(m_hwnd, SW_HIDE);
    }
    m_targetDialog = nullptr;
    m_targetProcessId = 0;
    m_targetProcess.clear();
    m_activeExplorerPath.clear();
    m_menuOpen = false;
}

bool DialogRibbonOverlay::doUpdatePosition() {
    std::lock_guard lock(m_mutex);
    try {
        if (!m_targetDialog || !IsWindow(m_targetDialog) || !m_hwnd ||
            m_targetProcessId == 0) {
            doHideLocked();
            return false;
        }

        wchar_t className[64]{};
        DWORD currentProcessId = 0;
        GetWindowThreadProcessId(m_targetDialog, &currentProcessId);
        if (currentProcessId != m_targetProcessId ||
            GetClassNameW(m_targetDialog, className, static_cast<int>(std::size(className))) <= 0 ||
            wcscmp(className, L"#32770") != 0) {
            // HWND values can be reused after a dialog is destroyed. PID plus
            // dialog class validation prevents following the replacement window.
            doHideLocked();
            return false;
        }
        const std::string currentProcess =
            tools3000::core::WinUtils::getProcessNameFromWindow(m_targetDialog);
        if (!m_targetProcess.empty() &&
            (currentProcess.empty() ||
             _stricmp(currentProcess.c_str(), m_targetProcess.c_str()) != 0)) {
            doHideLocked();
            return false;
        }

        // 动态验证目标对话框是否仍为合法的文件对话框，杜绝 HWND 复用为非文件弹窗
        if (!DialogNavigator::isFileDialog(m_targetDialog)) {
            doHideLocked();
            return false;
        }

        // 如果下拉菜单正在弹出交互中，始终保持展示
        if (m_menuOpen) {
            return true;
        }

        // 1. 检查目标对话框是否最小化或已被隐藏
        if (IsIconic(m_targetDialog) || !IsWindowVisible(m_targetDialog)) {
            if (IsWindowVisible(m_hwnd)) ShowWindow(m_hwnd, SW_HIDE);
            return false;
        }

        // 2. 前台归属硬校验：胶囊只属于文件对话框本身。
        // 切回同一 EXE 的主窗口（如 VS Code 主编辑器）或其他窗口时必须立刻隐藏，
        // 绝不能把“同进程”误当作“同一个文件对话框”。
        const HWND foreground = GetForegroundWindow();
        const HWND foregroundRoot = foreground ? GetAncestor(foreground, GA_ROOT) : nullptr;
        if (foreground != m_hwnd && foreground != m_targetDialog &&
            foregroundRoot != m_targetDialog) {
            if (IsWindowVisible(m_hwnd)) ShowWindow(m_hwnd, SW_HIDE);
            return false;
        }

        RECT dlgRect;
        if (!GetWindowRect(m_targetDialog, &dlgRect)) {
            doHideLocked();
            return false;
        }

        m_dpiScale = tools3000::core::dpi::scaleForWindow(m_targetDialog);
        updateButtonsLayout(m_dpiScale);

        int posX = 0;
        int posY = 0;
        std::string posConfig = PathMemoryManager::instance().getRibbonPosition();

        if (posConfig == "top-center") {
            posX = dlgRect.left + ((dlgRect.right - dlgRect.left) - m_width) / 2;
            posY = dlgRect.top + static_cast<int>(5.0f * m_dpiScale);
        } else {
            // top-right: 贴合在对话框右上角标题栏（位于系统最小化/关闭按钮左侧）
            posX = dlgRect.right - m_width - static_cast<int>(145.0f * m_dpiScale);
            posY = dlgRect.top + static_cast<int>(5.0f * m_dpiScale);
        }

        // 防溢出保护：防止小尺寸或窄窗口导致左侧越界
        if (posX < dlgRect.left + static_cast<int>(8.0f * m_dpiScale)) {
            posX = dlgRect.left + static_cast<int>(8.0f * m_dpiScale);
        }

        SetWindowPos(m_hwnd, HWND_TOPMOST, posX, posY, m_width, m_height,
                     SWP_NOACTIVATE | SWP_SHOWWINDOW);
        render();
        return true;
    } catch (const std::exception& e) {
        LOG_WARN("doUpdatePosition 异常: {}", e.what());
    } catch (...) {
        LOG_WARN("doUpdatePosition 未知异常");
    }
    doHideLocked();
    return false;
}

void DialogRibbonOverlay::ensureTextFormats(float dpiScale) {
    if (m_dwriteFactory && (m_textFormat == nullptr || std::abs(m_currentFontDpi - dpiScale) > 0.001f)) {
        m_textFormat.Reset();
        m_boldFormat.Reset();
        m_badgeFormat.Reset();
        m_currentFontDpi = dpiScale;

        // 全局单一事实源字体栈优先序列
        const wchar_t* preferredFonts[] = {
            L"Noto Sans SC",
            L"Source Han Sans SC",
            L"Segoe UI Variable Text",
            L"Segoe UI",
            L"PingFang SC",
            L"Microsoft YaHei UI",
            L"Microsoft YaHei"
        };

        float fontSize = 12.0f * dpiScale;
        float badgeSize = 10.0f * dpiScale;

        for (const auto* fontName : preferredFonts) {
            HRESULT hr = m_dwriteFactory->CreateTextFormat(
                fontName,
                nullptr,
                DWRITE_FONT_WEIGHT_MEDIUM,
                DWRITE_FONT_STYLE_NORMAL,
                DWRITE_FONT_STRETCH_NORMAL,
                fontSize,
                L"zh-CN",
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
                    L"zh-CN",
                    m_boldFormat.GetAddressOf()
                );
                m_dwriteFactory->CreateTextFormat(
                    fontName,
                    nullptr,
                    DWRITE_FONT_WEIGHT_BOLD,
                    DWRITE_FONT_STYLE_NORMAL,
                    DWRITE_FONT_STRETCH_NORMAL,
                    badgeSize,
                    L"zh-CN",
                    m_badgeFormat.GetAddressOf()
                );
                break;
            }
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

void DialogRibbonOverlay::updateButtonsLayout(float dpiScale) {
    m_buttons.clear();
    ensureTextFormats(dpiScale);

    try {
        m_activeExplorerPath = ExplorerTracker::instance().getActiveExplorerPath();
    } catch (...) {
        m_activeExplorerPath.clear();
    }

    auto measureText = [&](const std::string& utf8Text, bool isBold) -> float {
        std::wstring wText = tools3000::core::WinUtils::utf8ToWstring(utf8Text);
        if (wText.empty()) return 0.0f;
        auto format = isBold ? m_boldFormat.Get() : m_textFormat.Get();
        if (!format || !m_dwriteFactory) {
            return static_cast<float>(wText.length()) * 8.0f * dpiScale;
        }
        ComPtr<IDWriteTextLayout> layout;
        HRESULT hr = m_dwriteFactory->CreateTextLayout(
            wText.c_str(), static_cast<UINT32>(wText.length()),
            format, 1000.0f, 1000.0f, layout.GetAddressOf()
        );
        if (SUCCEEDED(hr) && layout) {
            DWRITE_TEXT_METRICS metrics{};
            layout->GetMetrics(&metrics);
            return metrics.widthIncludingTrailingWhitespace;
        }
        return static_cast<float>(wText.length()) * 8.0f * dpiScale;
    };

    m_height = static_cast<int>(32.0f * dpiScale);
    float paddingX = 6.0f * dpiScale;
    float currentX = paddingX;
    float btnH = 24.0f * dpiScale;
    float btnY = ((float)m_height - btnH) * 0.5f;

    // 1. 资源管理器同频胶囊 (左侧矢量文件夹图标 📁 + 目录名称)
    if (PathMemoryManager::instance().isQuickSwitchEnabled() && !m_activeExplorerPath.empty()) {
        std::string folderName = m_activeExplorerPath;
        size_t lastSlash = folderName.find_last_of("\\/");
        if (lastSlash != std::string::npos && lastSlash + 1 < folderName.length()) {
            folderName = folderName.substr(lastSlash + 1);
        }
        if (folderName.empty()) folderName = m_activeExplorerPath;

        std::string label = folderName;
        float textW = measureText(label, true);
        float iconW = 12.0f * dpiScale;
        float btnW = std::clamp(textW + iconW + 22.0f * dpiScale, 70.0f * dpiScale, 185.0f * dpiScale);

        m_buttons.push_back({
            RibbonButtonRect::Type::ExplorerSync,
            D2D1::RectF(currentX, btnY, currentX + btnW, btnY + btnH),
            label,
            m_activeExplorerPath,
            0,
            false
        });
        currentX += btnW + 10.0f * dpiScale; // 预留给微晶垂直分割线的空间
    }

    // 2. 最近使用 (矢量时钟 🕒 + "最近" + 独立微晶数字气泡 + 矢量 Chevron ⌵)
    try {
        auto recentList = PathMemoryManager::instance().getRecentPaths(5);
        std::string recentLabel = "最近";
        float textW = measureText(recentLabel, false);
        float iconW = 11.5f * dpiScale;
        float badgeW = 16.0f * dpiScale;
        float chevronW = 6.0f * dpiScale;
        float recentW = iconW + textW + badgeW + chevronW + 24.0f * dpiScale;

        m_buttons.push_back({
            RibbonButtonRect::Type::Recent,
            D2D1::RectF(currentX, btnY, currentX + recentW, btnY + btnH),
            recentLabel,
            "",
            static_cast<int>(recentList.size()),
            false
        });
        currentX += recentW + 4.0f * dpiScale;
    } catch (...) {
    }

    // 3. 常用收藏 (矢量书签 📌 + "工作区" + 矢量 Chevron ⌵)
    std::string favLabel = "工作区";
    float textW = measureText(favLabel, false);
    float iconW = 11.5f * dpiScale;
    float chevronW = 6.0f * dpiScale;
    float favW = iconW + textW + chevronW + 22.0f * dpiScale;

    m_buttons.push_back({
        RibbonButtonRect::Type::Favorites,
        D2D1::RectF(currentX, btnY, currentX + favW, btnY + btnH),
        favLabel,
        "",
        0,
        false
    });
    currentX += favW + paddingX;

    m_width = static_cast<int>(currentX);
}

void DialogRibbonOverlay::render() {
    if (!m_hwnd || !m_memDC || m_width <= 0 || m_height <= 0) return;

    if (m_memBitmap && (m_renderedWidth != m_width || m_renderedHeight != m_height)) {
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
        bmi.bmiHeader.biWidth = m_width;
        bmi.bmiHeader.biHeight = -m_height;
        bmi.bmiHeader.biPlanes = 1;
        bmi.bmiHeader.biBitCount = 32;
        bmi.bmiHeader.biCompression = BI_RGB;
        void* bits = nullptr;
        m_memBitmap = CreateDIBSection(screenDC, &bmi, DIB_RGB_COLORS, &bits, nullptr, 0);
        ReleaseDC(nullptr, screenDC);
        m_oldBitmap = static_cast<HBITMAP>(SelectObject(m_memDC, m_memBitmap));
        m_renderedWidth = m_width;
        m_renderedHeight = m_height;
    }

    if (!m_renderTarget && m_d2dFactory) {
        D2D1_RENDER_TARGET_PROPERTIES rtProps = D2D1::RenderTargetProperties(
            D2D1_RENDER_TARGET_TYPE_DEFAULT,
            D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED),
            0.0f, 0.0f,
            D2D1_RENDER_TARGET_USAGE_GDI_COMPATIBLE
        );
        m_d2dFactory->CreateDCRenderTarget(&rtProps, m_renderTarget.GetAddressOf());
    }

    if (!m_renderTarget) return;

    ensureTextFormats(m_dpiScale);

    RECT rc = {0, 0, m_width, m_height};
    m_renderTarget->BindDC(m_memDC, &rc);
    m_renderTarget->BeginDraw();
    m_renderTarget->Clear(D2D1::ColorF(0, 0, 0, 0));

    // 自适应系统深浅色主题检测
    bool isDark = tools3000::core::WinUtils::isSystemDarkMode();

    ComPtr<ID2D1SolidColorBrush> bgBrush;
    ComPtr<ID2D1SolidColorBrush> borderBrush;
    ComPtr<ID2D1SolidColorBrush> textBrush;
    ComPtr<ID2D1SolidColorBrush> textMutedBrush;
    ComPtr<ID2D1SolidColorBrush> highlightBrush;
    ComPtr<ID2D1SolidColorBrush> highlightBorderBrush;
    ComPtr<ID2D1SolidColorBrush> syncBgBrush;
    ComPtr<ID2D1SolidColorBrush> syncBorderBrush;
    ComPtr<ID2D1SolidColorBrush> chevronBrush;
    ComPtr<ID2D1SolidColorBrush> badgeBgBrush;
    ComPtr<ID2D1SolidColorBrush> badgeTextBrush;
    ComPtr<ID2D1SolidColorBrush> dividerBrush;
    ComPtr<ID2D1SolidColorBrush> whiteBrush;

    m_renderTarget->CreateSolidColorBrush(D2D1::ColorF(1.0f, 1.0f, 1.0f, 1.0f), whiteBrush.GetAddressOf());

    if (isDark) {
        // 深色亚克力模式
        m_renderTarget->CreateSolidColorBrush(D2D1::ColorF(0.12f, 0.13f, 0.17f, 0.93f), bgBrush.GetAddressOf());
        m_renderTarget->CreateSolidColorBrush(D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.13f), borderBrush.GetAddressOf());
        m_renderTarget->CreateSolidColorBrush(D2D1::ColorF(0.96f, 0.97f, 0.98f, 0.96f), textBrush.GetAddressOf());
        m_renderTarget->CreateSolidColorBrush(D2D1::ColorF(0.72f, 0.76f, 0.82f, 0.88f), textMutedBrush.GetAddressOf());
        m_renderTarget->CreateSolidColorBrush(D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.09f), highlightBrush.GetAddressOf());
        m_renderTarget->CreateSolidColorBrush(D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.15f), highlightBorderBrush.GetAddressOf());
        m_renderTarget->CreateSolidColorBrush(D2D1::ColorF(0.18f, 0.48f, 0.94f, 0.95f), syncBgBrush.GetAddressOf());
        m_renderTarget->CreateSolidColorBrush(D2D1::ColorF(0.40f, 0.68f, 1.0f, 0.40f), syncBorderBrush.GetAddressOf());
        m_renderTarget->CreateSolidColorBrush(D2D1::ColorF(0.70f, 0.74f, 0.82f, 0.75f), chevronBrush.GetAddressOf());
        m_renderTarget->CreateSolidColorBrush(D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.12f), badgeBgBrush.GetAddressOf());
        m_renderTarget->CreateSolidColorBrush(D2D1::ColorF(0.95f, 0.95f, 0.98f, 0.95f), badgeTextBrush.GetAddressOf());
        m_renderTarget->CreateSolidColorBrush(D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.12f), dividerBrush.GetAddressOf());
    } else {
        // 浅色白雾磨砂模式（与浅色文件对话框 100% 浑然天成）
        m_renderTarget->CreateSolidColorBrush(D2D1::ColorF(0.965f, 0.975f, 0.985f, 0.96f), bgBrush.GetAddressOf());
        m_renderTarget->CreateSolidColorBrush(D2D1::ColorF(0.0f, 0.0f, 0.0f, 0.09f), borderBrush.GetAddressOf());
        m_renderTarget->CreateSolidColorBrush(D2D1::ColorF(0.10f, 0.12f, 0.16f, 0.95f), textBrush.GetAddressOf());
        m_renderTarget->CreateSolidColorBrush(D2D1::ColorF(0.32f, 0.36f, 0.44f, 0.88f), textMutedBrush.GetAddressOf());
        m_renderTarget->CreateSolidColorBrush(D2D1::ColorF(0.0f, 0.0f, 0.0f, 0.05f), highlightBrush.GetAddressOf());
        m_renderTarget->CreateSolidColorBrush(D2D1::ColorF(0.0f, 0.0f, 0.0f, 0.08f), highlightBorderBrush.GetAddressOf());
        m_renderTarget->CreateSolidColorBrush(D2D1::ColorF(0.12f, 0.44f, 0.90f, 0.95f), syncBgBrush.GetAddressOf());
        m_renderTarget->CreateSolidColorBrush(D2D1::ColorF(0.20f, 0.55f, 1.0f, 0.50f), syncBorderBrush.GetAddressOf());
        m_renderTarget->CreateSolidColorBrush(D2D1::ColorF(0.40f, 0.44f, 0.52f, 0.75f), chevronBrush.GetAddressOf());
        m_renderTarget->CreateSolidColorBrush(D2D1::ColorF(0.0f, 0.0f, 0.0f, 0.06f), badgeBgBrush.GetAddressOf());
        m_renderTarget->CreateSolidColorBrush(D2D1::ColorF(0.18f, 0.22f, 0.28f, 0.92f), badgeTextBrush.GetAddressOf());
        m_renderTarget->CreateSolidColorBrush(D2D1::ColorF(0.0f, 0.0f, 0.0f, 0.09f), dividerBrush.GetAddressOf());
    }

    // 绘制主体全圆角微晶磨砂胶囊 (Full Pill Capsule)
    float capsuleRadius = static_cast<float>(m_height) * 0.5f;
    D2D1_ROUNDED_RECT mainRRect = D2D1::RoundedRect(
        D2D1::RectF(0.0f, 0.0f, static_cast<float>(m_width), static_cast<float>(m_height)),
        capsuleRadius, capsuleRadius
    );

    if (bgBrush) m_renderTarget->FillRoundedRectangle(&mainRRect, bgBrush.Get());
    if (borderBrush) m_renderTarget->DrawRoundedRectangle(&mainRRect, borderBrush.Get(), 1.0f * m_dpiScale);

    // 绘制各个按钮与微交互
    for (size_t i = 0; i < m_buttons.size(); ++i) {
        const auto& btn = m_buttons[i];
        float btnRadius = (btn.rect.bottom - btn.rect.top) * 0.5f;
        D2D1_ROUNDED_RECT btnRRect = D2D1::RoundedRect(btn.rect, btnRadius, btnRadius);

        if (btn.type == RibbonButtonRect::Type::ExplorerSync && syncBgBrush) {
            // 1. 同步路径主胶囊
            syncBgBrush->SetOpacity(btn.hovered ? 1.0f : 0.92f);
            m_renderTarget->FillRoundedRectangle(&btnRRect, syncBgBrush.Get());
            if (syncBorderBrush) {
                m_renderTarget->DrawRoundedRectangle(&btnRRect, syncBorderBrush.Get(), 1.0f * m_dpiScale);
            }

            // 矢量文件夹图标 📁 (纯白)
            float iconSize = 12.0f * m_dpiScale;
            float iconX = btn.rect.left + 8.0f * m_dpiScale;
            float iconY = (btn.rect.top + btn.rect.bottom - iconSize) * 0.5f;
            drawFolderIcon(m_renderTarget.Get(), whiteBrush.Get(), iconX, iconY, iconSize, 1.2f * m_dpiScale);

            // 文字居中偏右
            std::wstring wText = tools3000::core::WinUtils::utf8ToWstring(btn.text);
            if (whiteBrush && m_boldFormat) {
                D2D1_RECT_F textRect = D2D1::RectF(
                    iconX + iconSize + 5.0f * m_dpiScale, btn.rect.top,
                    btn.rect.right - 8.0f * m_dpiScale, btn.rect.bottom
                );
                m_boldFormat->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
                m_boldFormat->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
                m_renderTarget->DrawText(
                    wText.c_str(), static_cast<UINT32>(wText.length()),
                    m_boldFormat.Get(), textRect, whiteBrush.Get()
                );
            }

            // 在同步胶囊右侧绘制垂直微晶分割线
            if (dividerBrush) {
                float divX = btn.rect.right + 5.0f * m_dpiScale;
                float divY1 = (static_cast<float>(m_height) - 14.0f * m_dpiScale) * 0.5f;
                float divY2 = divY1 + 14.0f * m_dpiScale;
                m_renderTarget->DrawLine(
                    D2D1::Point2F(divX, divY1),
                    D2D1::Point2F(divX, divY2),
                    dividerBrush.Get(), 1.0f * m_dpiScale
                );
            }
        } else if (btn.type == RibbonButtonRect::Type::Recent) {
            // 2. 最近使用项 (时钟图标 + 文字 + 独立数字徽章 + Chevron)
            if (btn.hovered && highlightBrush) {
                m_renderTarget->FillRoundedRectangle(&btnRRect, highlightBrush.Get());
                if (highlightBorderBrush) {
                    m_renderTarget->DrawRoundedRectangle(&btnRRect, highlightBorderBrush.Get(), 1.0f * m_dpiScale);
                }
            }

            auto activeTextBrush = btn.hovered ? textBrush.Get() : textMutedBrush.Get();
            auto activeChevronBrush = btn.hovered ? textBrush.Get() : chevronBrush.Get();

            // 矢量时钟图标 🕒
            float iconSize = 11.5f * m_dpiScale;
            float iconX = btn.rect.left + 7.0f * m_dpiScale;
            float iconY = (btn.rect.top + btn.rect.bottom - iconSize) * 0.5f;
            drawClockIcon(m_renderTarget.Get(), activeTextBrush, iconX, iconY, iconSize, 1.15f * m_dpiScale);

            // 文字 "最近"
            std::wstring wText = tools3000::core::WinUtils::utf8ToWstring(btn.text);
            float textLeft = iconX + iconSize + 4.5f * m_dpiScale;
            float textW = 28.0f * m_dpiScale;
            if (activeTextBrush && m_textFormat) {
                D2D1_RECT_F textRect = D2D1::RectF(textLeft, btn.rect.top, textLeft + textW, btn.rect.bottom);
                m_textFormat->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
                m_textFormat->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
                m_renderTarget->DrawText(
                    wText.c_str(), static_cast<UINT32>(wText.length()),
                    m_textFormat.Get(), textRect, activeTextBrush
                );
            }

            // 独立数字微晶气泡
            float badgeW = 16.0f * m_dpiScale;
            float badgeH = 14.0f * m_dpiScale;
            float badgeX = textLeft + textW + 3.0f * m_dpiScale;
            float badgeY = (btn.rect.top + btn.rect.bottom - badgeH) * 0.5f;
            drawBadge(m_renderTarget.Get(), badgeBgBrush.Get(), badgeTextBrush.Get(), badgeX, badgeY, badgeW, badgeH, btn.badgeCount);

            // 矢量微型 ChevronDown 箭头
            float chevronCenterX = btn.rect.right - 7.0f * m_dpiScale;
            float chevronCenterY = (btn.rect.top + btn.rect.bottom) * 0.5f;
            drawChevron(
                m_renderTarget.Get(), activeChevronBrush,
                chevronCenterX, chevronCenterY,
                5.5f * m_dpiScale, 1.2f * m_dpiScale
            );
        } else if (btn.type == RibbonButtonRect::Type::Favorites) {
            // 3. 工作区收藏项 (书签图标 + 文字 + Chevron)
            if (btn.hovered && highlightBrush) {
                m_renderTarget->FillRoundedRectangle(&btnRRect, highlightBrush.Get());
                if (highlightBorderBrush) {
                    m_renderTarget->DrawRoundedRectangle(&btnRRect, highlightBorderBrush.Get(), 1.0f * m_dpiScale);
                }
            }

            auto activeTextBrush = btn.hovered ? textBrush.Get() : textMutedBrush.Get();
            auto activeChevronBrush = btn.hovered ? textBrush.Get() : chevronBrush.Get();

            // 矢量书签图标 📌
            float iconSize = 11.0f * m_dpiScale;
            float iconX = btn.rect.left + 7.0f * m_dpiScale;
            float iconY = (btn.rect.top + btn.rect.bottom - iconSize) * 0.5f;
            drawBookmarkIcon(m_renderTarget.Get(), activeTextBrush, iconX, iconY, iconSize, 1.15f * m_dpiScale);

            // 文字 "工作区"
            std::wstring wText = tools3000::core::WinUtils::utf8ToWstring(btn.text);
            float textLeft = iconX + iconSize + 4.5f * m_dpiScale;
            float textW = 40.0f * m_dpiScale;
            if (activeTextBrush && m_textFormat) {
                D2D1_RECT_F textRect = D2D1::RectF(textLeft, btn.rect.top, textLeft + textW, btn.rect.bottom);
                m_textFormat->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
                m_textFormat->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
                m_renderTarget->DrawText(
                    wText.c_str(), static_cast<UINT32>(wText.length()),
                    m_textFormat.Get(), textRect, activeTextBrush
                );
            }

            // 矢量微型 ChevronDown 箭头
            float chevronCenterX = btn.rect.right - 7.0f * m_dpiScale;
            float chevronCenterY = (btn.rect.top + btn.rect.bottom) * 0.5f;
            drawChevron(
                m_renderTarget.Get(), activeChevronBrush,
                chevronCenterX, chevronCenterY,
                5.5f * m_dpiScale, 1.2f * m_dpiScale
            );
        }
    }

    m_renderTarget->EndDraw();

    // 提交分层窗口
    HDC screenDC = GetDC(nullptr);
    POINT ptSrc = {0, 0};
    RECT winRect;
    GetWindowRect(m_hwnd, &winRect);
    POINT ptDst = {winRect.left, winRect.top};
    SIZE size = {m_width, m_height};
    BLENDFUNCTION blend = {AC_SRC_OVER, 0, 255, AC_SRC_ALPHA};

    UpdateLayeredWindow(m_hwnd, screenDC, &ptDst, &size, m_memDC, &ptSrc, 0, &blend, ULW_ALPHA);
    ReleaseDC(nullptr, screenDC);
}

void DialogRibbonOverlay::onMouseMove(int x, int y) {
    if (!m_trackingMouse) {
        TRACKMOUSEEVENT tme = {sizeof(TRACKMOUSEEVENT), TME_LEAVE, m_hwnd, 0};
        TrackMouseEvent(&tme);
        m_trackingMouse = true;
    }

    bool needRepaint = false;
    for (auto& btn : m_buttons) {
        bool inside = (x >= btn.rect.left && x <= btn.rect.right &&
                       y >= btn.rect.top && y <= btn.rect.bottom);
        if (btn.hovered != inside) {
            btn.hovered = inside;
            needRepaint = true;
        }
    }

    if (needRepaint) {
        render();
    }
}

void DialogRibbonOverlay::onMouseLeave() {
    m_trackingMouse = false;
    bool needRepaint = false;
    for (auto& btn : m_buttons) {
        if (btn.hovered) {
            btn.hovered = false;
            needRepaint = true;
        }
    }
    if (needRepaint) render();
}

void DialogRibbonOverlay::onLButtonDown(int x, int y) {
    for (const auto& btn : m_buttons) {
        if (x >= btn.rect.left && x <= btn.rect.right &&
            y >= btn.rect.top && y <= btn.rect.bottom) {

            POINT screenPt = {static_cast<int>(btn.rect.left), static_cast<int>(btn.rect.bottom)};
            ClientToScreen(m_hwnd, &screenPt);

            if (btn.type == RibbonButtonRect::Type::ExplorerSync) {
                const HWND targetDialog = getTargetDialog();
                if (!btn.extraData.empty() && targetDialog) {
                    DialogNavigator::instance().navigateToFolder(targetDialog, btn.extraData);
                    updateButtonsLayout(m_dpiScale);
                    render();
                }
            } else if (btn.type == RibbonButtonRect::Type::Recent) {
                showRecentMenu(screenPt.x, screenPt.y + 4);
            } else if (btn.type == RibbonButtonRect::Type::Favorites) {
                showFavoritesMenu(screenPt.x, screenPt.y + 4);
            }
            break;
        }
    }
}

void DialogRibbonOverlay::showRecentMenu(int screenX, int screenY) {
    auto recentList = PathMemoryManager::instance().getRecentPaths(10);
    if (recentList.empty()) return;

    HMENU hMenu = CreatePopupMenu();
    for (size_t i = 0; i < recentList.size(); ++i) {
        std::wstring itemText = tools3000::core::WinUtils::utf8ToWstring(recentList[i]);
        AppendMenuW(hMenu, MF_STRING, static_cast<UINT_PTR>(1000 + i), itemText.c_str());
    }

    m_menuOpen = true;
    SetForegroundWindow(m_hwnd);
    int cmd = TrackPopupMenu(
        hMenu, TPM_LEFTALIGN | TPM_TOPALIGN | TPM_RETURNCMD | TPM_NONOTIFY,
        screenX, screenY, 0, m_hwnd, nullptr
    );
    DestroyMenu(hMenu);
    m_menuOpen = false;

    if (cmd >= 1000 && static_cast<size_t>(cmd - 1000) < recentList.size()) {
        std::string chosenPath = recentList[cmd - 1000];
        const HWND targetDialog = getTargetDialog();
        if (targetDialog) {
            DialogNavigator::instance().navigateToFolder(targetDialog, chosenPath);
            updateButtonsLayout(m_dpiScale);
            render();
        }
    }
}

void DialogRibbonOverlay::showFavoritesMenu(int screenX, int screenY) {
    auto favList = PathMemoryManager::instance().getFavorites();
    if (favList.empty()) return;

    HMENU hMenu = CreatePopupMenu();
    for (size_t i = 0; i < favList.size(); ++i) {
        std::wstring itemText = tools3000::core::WinUtils::utf8ToWstring(favList[i]);
        AppendMenuW(hMenu, MF_STRING, static_cast<UINT_PTR>(2000 + i), itemText.c_str());
    }

    m_menuOpen = true;
    SetForegroundWindow(m_hwnd);
    int cmd = TrackPopupMenu(
        hMenu, TPM_LEFTALIGN | TPM_TOPALIGN | TPM_RETURNCMD | TPM_NONOTIFY,
        screenX, screenY, 0, m_hwnd, nullptr
    );
    DestroyMenu(hMenu);
    m_menuOpen = false;

    if (cmd >= 2000 && static_cast<size_t>(cmd - 2000) < favList.size()) {
        std::string chosenPath = favList[cmd - 2000];
        const HWND targetDialog = getTargetDialog();
        if (targetDialog) {
            DialogNavigator::instance().navigateToFolder(targetDialog, chosenPath);
            updateButtonsLayout(m_dpiScale);
            render();
        }
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
                self->doUpdatePosition();
                return 0;
            case WM_RIBBON_ATTACH:
                self->doAttachToDialog();
                return 0;
            case WM_RIBBON_HIDE:
                self->doHide();
                return 0;
            case WM_MOUSEMOVE:
                self->onMouseMove(LOWORD(lParam), HIWORD(lParam));
                return 0;
            case WM_MOUSELEAVE:
                self->onMouseLeave();
                return 0;
            case WM_LBUTTONDOWN:
                self->onLButtonDown(LOWORD(lParam), HIWORD(lParam));
                return 0;
            case WM_SETCURSOR:
                SetCursor(LoadCursorW(nullptr, IDC_HAND));
                return TRUE;
        }
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

} // namespace tools3000::dialog
