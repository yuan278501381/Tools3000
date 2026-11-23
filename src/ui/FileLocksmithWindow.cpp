#include "ui/FileLocksmithWindow.h"
#include "core/config/ConfigManager.h"
#include "core/i18n/UiI18n.h"
#include "core/utils/DWriteFontUtils.h"
#include "core/utils/DpiUtils.h"
#include "core/utils/FileLocksmithEngine.h"
#include "core/utils/ThemeUtils.h"
#include "core/utils/WinUtils.h"

#include "../resource.h"

#include <d2d1.h>
#include <dwrite.h>
#include <windowsx.h>
#include <wrl/client.h>

#include <algorithm>
#include <atomic>
#include <filesystem>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

using Microsoft::WRL::ComPtr;

namespace tools3000::ui {

namespace {

constexpr const wchar_t* kLocksmithWindowClass = L"Tools3000_FileLocksmithWindow";
constexpr UINT WM_LOCKSMITH_SCAN_DONE = WM_APP + 61;

struct HitButton {
    D2D1_RECT_F rect{};
    int kind = 0;        // 1=close, 2=rescan, 3=elevate, 4=endProc, 5=restartExplorer, 6=locateProc
    DWORD pid = 0;
    std::wstring path;
    bool disabled = false;
};

struct LocksmithColors {
    D2D1_COLOR_F bg;
    D2D1_COLOR_F surface;
    D2D1_COLOR_F surfaceHover;
    D2D1_COLOR_F border;
    D2D1_COLOR_F textPrimary;
    D2D1_COLOR_F textSecondary;
    D2D1_COLOR_F accent;
    D2D1_COLOR_F danger;
    D2D1_COLOR_F warn;
    D2D1_COLOR_F ok;
};

LocksmithColors buildColors(bool dark) noexcept {
    if (dark) {
        return {
            D2D1::ColorF(0x16181E, 1.0f),
            D2D1::ColorF(0x1F232C, 1.0f),
            D2D1::ColorF(0x2A303C, 1.0f),
            D2D1::ColorF(0x323947, 1.0f),
            D2D1::ColorF(0xF1F5F9, 1.0f),
            D2D1::ColorF(0x94A3B8, 1.0f),
            D2D1::ColorF(0x3B82F6, 1.0f),
            D2D1::ColorF(0xEF4444, 1.0f),
            D2D1::ColorF(0xF59E0B, 1.0f),
            D2D1::ColorF(0x10B981, 1.0f)
        };
    }
    return {
        D2D1::ColorF(0xF8FAFC, 1.0f),
        D2D1::ColorF(0xFFFFFF, 1.0f),
        D2D1::ColorF(0xF1F5F9, 1.0f),
        D2D1::ColorF(0xCBD5E1, 1.0f),
        D2D1::ColorF(0x0F172A, 1.0f),
        D2D1::ColorF(0x475569, 1.0f),
        D2D1::ColorF(0x2563EB, 1.0f),
        D2D1::ColorF(0xDC2626, 1.0f),
        D2D1::ColorF(0xD97706, 1.0f),
        D2D1::ColorF(0x059669, 1.0f)
    };
}

class FileLocksmithDialog {
public:
    explicit FileLocksmithDialog(std::wstring targetPath)
        : m_targetPath(std::move(targetPath)),
          m_dark(tools3000::core::NativeThemePalette::isEffectiveDarkMode()) {
        D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, m_d2dFactory.GetAddressOf());
        DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory),
                            reinterpret_cast<IUnknown**>(m_dwriteFactory.GetAddressOf()));
    }

    ~FileLocksmithDialog() {
        m_closing.store(true);
        if (m_scanThread.joinable()) {
            m_scanThread.join();
        }
    }

    int run(HINSTANCE hInstance) {
        const HMONITOR monitor = tools3000::core::dpi::activeMonitor();
        const RECT work = tools3000::core::dpi::workArea(monitor);
        const unsigned dpi = tools3000::core::dpi::effectiveDpiForMonitor(monitor);
        m_scale = tools3000::core::dpi::scaleForDpi(dpi);

        WNDCLASSEXW wc{};
        wc.cbSize = sizeof(wc);
        wc.style = CS_HREDRAW | CS_VREDRAW;
        wc.lpfnWndProc = &FileLocksmithDialog::wndProc;
        wc.hInstance = hInstance;
        wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
        wc.lpszClassName = kLocksmithWindowClass;
        wc.hIcon = static_cast<HICON>(LoadImageW(
            hInstance, MAKEINTRESOURCEW(IDI_APP_ICON), IMAGE_ICON,
            GetSystemMetricsForDpi(SM_CXICON, dpi), GetSystemMetricsForDpi(SM_CYICON, dpi),
            LR_DEFAULTCOLOR | LR_SHARED));
        RegisterClassExW(&wc);

        const int w = tools3000::core::dpi::scaleMetric(680, m_scale);
        const int h = tools3000::core::dpi::scaleMetric(500, m_scale);
        const int x = work.left + (work.right - work.left - w) / 2;
        const int y = work.top + (work.bottom - work.top - h) / 2;

        const std::wstring title = tools3000::core::trW(L"文件占用解锁 - Tools3000", L"File Locksmith - Tools3000");
        m_hwnd = CreateWindowExW(
            WS_EX_APPWINDOW,
            kLocksmithWindowClass,
            title.c_str(),
            WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN,
            x, y, w, h,
            nullptr, nullptr, hInstance, this);
        if (!m_hwnd) return 1;

        tools3000::core::WinUtils::applyImmersiveDarkMode(m_hwnd, m_dark);
        SetWindowPos(m_hwnd, nullptr, 0, 0, 0, 0,
                     SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE | SWP_FRAMECHANGED);
        tools3000::core::WinUtils::applyUniversalRoundedCorners(
            m_hwnd, w, h, tools3000::core::dpi::scaleMetric(12, m_scale));

        ShowWindow(m_hwnd, SW_SHOW);
        SetForegroundWindow(m_hwnd);
        startScanAsync();

        MSG msg{};
        while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
        return 0;
    }

private:
    void ensureResources() {
        if (!m_d2dFactory || !m_hwnd) return;
        RECT rc{};
        GetClientRect(m_hwnd, &rc);
        const D2D1_SIZE_U pxSize = D2D1::SizeU(
            static_cast<UINT32>((std::max)(1L, rc.right - rc.left)),
            static_cast<UINT32>((std::max)(1L, rc.bottom - rc.top)));
        if (!m_rt) {
            auto props = D2D1::RenderTargetProperties();
            props.dpiX = 96.0f * m_scale;
            props.dpiY = 96.0f * m_scale;
            m_d2dFactory->CreateHwndRenderTarget(
                props,
                D2D1::HwndRenderTargetProperties(m_hwnd, pxSize),
                &m_rt);
            if (m_rt) {
                m_rt->CreateSolidColorBrush(D2D1::ColorF(D2D1::ColorF::White), &m_brush);
            }
        } else {
            m_rt->SetDpi(96.0f * m_scale, 96.0f * m_scale);
            m_rt->Resize(pxSize);
        }

        if (m_dwriteFactory && !m_titleFormat) {
            const std::wstring locale = tools3000::core::UiI18n::toLocaleNameW(
                tools3000::core::UiI18n::getEffectiveLanguage());
            const wchar_t* fontName = L"Microsoft YaHei UI";
            m_dwriteFactory->CreateTextFormat(
                fontName, nullptr, DWRITE_FONT_WEIGHT_SEMI_BOLD, DWRITE_FONT_STYLE_NORMAL,
                DWRITE_FONT_STRETCH_NORMAL, 16.0f, locale.c_str(), &m_titleFormat);
            m_dwriteFactory->CreateTextFormat(
                fontName, nullptr, DWRITE_FONT_WEIGHT_MEDIUM, DWRITE_FONT_STYLE_NORMAL,
                DWRITE_FONT_STRETCH_NORMAL, 13.0f, locale.c_str(), &m_bodyFormat);
            m_dwriteFactory->CreateTextFormat(
                L"Consolas", nullptr, DWRITE_FONT_WEIGHT_MEDIUM, DWRITE_FONT_STYLE_NORMAL,
                DWRITE_FONT_STRETCH_NORMAL, 12.0f, locale.c_str(), &m_monoFormat);
            if (m_titleFormat) m_titleFormat->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
            if (m_bodyFormat) m_bodyFormat->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
            if (m_monoFormat) m_monoFormat->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
        }
    }

    void startScanAsync() {
        if (m_scanning.exchange(true)) return;
        if (m_scanThread.joinable()) m_scanThread.join();
        m_statusBanner.clear();
        InvalidateRect(m_hwnd, nullptr, FALSE);

        const std::wstring target = m_targetPath;
        m_scanThread = std::thread([this, target]() {
            auto res = tools3000::core::FileLocksmithEngine::scanTargetLocks(target, 3500);
            {
                std::lock_guard lock(m_resultMutex);
                m_scanResult = std::move(res);
            }
            m_scanning.store(false);
            if (!m_closing.load() && m_hwnd && IsWindow(m_hwnd)) {
                PostMessageW(m_hwnd, WM_LOCKSMITH_SCAN_DONE, 0, 0);
            }
        });
    }

    void paint() {
        ensureResources();
        if (!m_rt || !m_brush) return;

        const D2D1_SIZE_F sz = m_rt->GetSize();
        const auto c = buildColors(m_dark);
        m_buttons.clear();

        m_rt->BeginDraw();
        m_rt->Clear(c.bg);

        auto drawText = [&](const std::wstring& text, IDWriteTextFormat* fmt,
                            D2D1_RECT_F rect, D2D1_COLOR_F col,
                            DWRITE_TEXT_ALIGNMENT align = DWRITE_TEXT_ALIGNMENT_LEADING) {
            if (!fmt) return;
            fmt->SetTextAlignment(align);
            fmt->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
            m_brush->SetColor(col);
            m_rt->DrawTextW(text.c_str(), static_cast<UINT32>(text.size()), fmt, rect, m_brush.Get(),
                            D2D1_DRAW_TEXT_OPTIONS_CLIP);
        };

        auto drawBtn = [&](D2D1_RECT_F rect, const std::wstring& label, int kind,
                           D2D1_COLOR_F fill, D2D1_COLOR_F textCol,
                           DWORD pid = 0, std::wstring path = L"", bool disabled = false) {
            const bool hovered = !disabled &&
                m_mouseDip.x >= rect.left && m_mouseDip.x <= rect.right &&
                m_mouseDip.y >= rect.top && m_mouseDip.y <= rect.bottom;
            D2D1_COLOR_F bgCol = fill;
            if (hovered) {
                bgCol.a = (std::min)(1.0f, bgCol.a + 0.14f);
            }
            if (disabled) {
                bgCol.a *= 0.45f;
            }
            m_brush->SetColor(bgCol);
            m_rt->FillRoundedRectangle(D2D1::RoundedRect(rect, 6.0f, 6.0f), m_brush.Get());
            m_brush->SetColor(c.border);
            m_rt->DrawRoundedRectangle(D2D1::RoundedRect(rect, 6.0f, 6.0f), m_brush.Get(), 1.0f);
            drawText(label, m_bodyFormat.Get(), rect, textCol, DWRITE_TEXT_ALIGNMENT_CENTER);
            m_buttons.push_back({rect, kind, pid, std::move(path), disabled});
        };

        // 1. 顶栏
        const D2D1_RECT_F headerRect = D2D1::RectF(0, 0, sz.width, 64.0f);
        m_brush->SetColor(c.surface);
        m_rt->FillRectangle(headerRect, m_brush.Get());
        m_brush->SetColor(c.border);
        m_rt->DrawLine(D2D1::Point2F(0, 64.0f), D2D1::Point2F(sz.width, 64.0f), m_brush.Get(), 1.0f);

        const std::wstring headerTitle = tools3000::core::trW(L"文件占用检测与解锁", L"File Locksmith");
        drawText(headerTitle, m_titleFormat.Get(), D2D1::RectF(20, 10, sz.width - 70, 34), c.textPrimary);
        drawText(m_targetPath, m_monoFormat.Get(), D2D1::RectF(20, 36, sz.width - 70, 56), c.textSecondary);

        drawBtn(D2D1::RectF(sz.width - 44, 16, sz.width - 14, 46), L"X", 1,
                c.surfaceHover, c.textPrimary);

        // 2. 主体列表区
        const float listTop = 76.0f;
        const float listBottom = sz.height - 62.0f;
        tools3000::core::FileLockScanResult snap;
        {
            std::lock_guard lock(m_resultMutex);
            snap = m_scanResult;
        }

        if (m_scanning.load()) {
            const std::wstring scanningText = tools3000::core::trW(
                L"正在深度扫描占用该目标的进程与句柄...",
                L"Scanning processes and open handles locking this path...");
            drawText(scanningText, m_bodyFormat.Get(),
                     D2D1::RectF(24, listTop, sz.width - 24, listBottom),
                     c.textSecondary, DWRITE_TEXT_ALIGNMENT_CENTER);
        } else if (snap.processes.empty()) {
            const std::wstring emptyTitle = tools3000::core::trW(
                L"未检测到占用该文件或目录的进程",
                L"No processes were found locking this file or folder");
            drawText(emptyTitle, m_titleFormat.Get(),
                     D2D1::RectF(24, listTop + 40, sz.width - 24, listTop + 80),
                     c.ok, DWRITE_TEXT_ALIGNMENT_CENTER);
            if (!snap.currentProcessElevated && snap.inaccessibleProcessCount > 0) {
                const std::wstring hint = tools3000::core::trW(
                    L"提示：有系统或管理员进程无权读取，可点击下方「以管理员重新扫描」深度排查。",
                    L"Hint: Some elevated processes could not be inspected. Click 'Rescan as Administrator' below.");
                drawText(hint, m_bodyFormat.Get(),
                         D2D1::RectF(32, listTop + 88, sz.width - 32, listTop + 120),
                         c.warn, DWRITE_TEXT_ALIGNMENT_CENTER);
            }
        } else {
            m_rt->PushAxisAlignedClip(D2D1::RectF(0, listTop, sz.width, listBottom), D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);
            const float rowH = 66.0f;
            const float gap = 8.0f;
            const float totalContentH = static_cast<float>(snap.processes.size()) * (rowH + gap);
            const float maxScroll = (std::max)(0.0f, totalContentH - (listBottom - listTop));
            m_scrollY = std::clamp(m_scrollY, 0.0f, maxScroll);

            for (size_t i = 0; i < snap.processes.size(); ++i) {
                const auto& proc = snap.processes[i];
                const float y0 = listTop + static_cast<float>(i) * (rowH + gap) - m_scrollY;
                const float y1 = y0 + rowH;
                if (y1 < listTop || y0 > listBottom) continue;

                const D2D1_RECT_F card = D2D1::RectF(18, y0, sz.width - 18, y1);
                m_brush->SetColor(c.surface);
                m_rt->FillRoundedRectangle(D2D1::RoundedRect(card, 8.0f, 8.0f), m_brush.Get());
                m_brush->SetColor(c.border);
                m_rt->DrawRoundedRectangle(D2D1::RoundedRect(card, 8.0f, 8.0f), m_brush.Get(), 1.0f);

                const std::wstring titleLine = proc.displayName + L"  (PID: " + std::to_wstring(proc.pid) + L")";
                drawText(titleLine, m_titleFormat.Get(), D2D1::RectF(32, y0 + 8, sz.width - 230, y0 + 34), c.textPrimary);

                std::wstring subLine = !proc.exePath.empty() ? proc.exePath : proc.exeName;
                if (proc.risk == tools3000::core::LockProcessRisk::CriticalSystem) {
                    subLine = tools3000::core::trW(L"[系统关键进程 - 禁止强杀] ", L"[System Critical - Protected] ") + subLine;
                } else if (proc.risk == tools3000::core::LockProcessRisk::Explorer) {
                    subLine = tools3000::core::trW(L"[资源管理器 - 建议安全重启] ", L"[File Explorer - Safe Restart] ") + subLine;
                }
                drawText(subLine, m_monoFormat.Get(), D2D1::RectF(32, y0 + 36, sz.width - 230, y0 + 58), c.textSecondary);

                if (!proc.exePath.empty()) {
                    drawBtn(D2D1::RectF(sz.width - 216, y0 + 18, sz.width - 134, y0 + 48),
                            tools3000::core::trW(L"定位", L"Locate"), 6,
                            c.surfaceHover, c.textPrimary, proc.pid, proc.exePath);
                }

                if (proc.risk == tools3000::core::LockProcessRisk::Explorer) {
                    drawBtn(D2D1::RectF(sz.width - 126, y0 + 18, sz.width - 28, y0 + 48),
                            tools3000::core::trW(L"重启桌面", L"Restart"), 5,
                            c.warn, D2D1::ColorF(0xFFFFFF, 1.0f), proc.pid);
                } else if (proc.risk == tools3000::core::LockProcessRisk::CriticalSystem) {
                    drawBtn(D2D1::RectF(sz.width - 126, y0 + 18, sz.width - 28, y0 + 48),
                            tools3000::core::trW(L"受保护", L"Protected"), 4,
                            c.surfaceHover, c.textSecondary, proc.pid, L"", true);
                } else {
                    drawBtn(D2D1::RectF(sz.width - 126, y0 + 18, sz.width - 28, y0 + 48),
                            tools3000::core::trW(L"结束进程", L"End Task"), 4,
                            c.danger, D2D1::ColorF(0xFFFFFF, 1.0f), proc.pid);
                }
            }
            m_rt->PopAxisAlignedClip();
        }

        // 3. 底栏
        const D2D1_RECT_F footerRect = D2D1::RectF(0, sz.height - 54.0f, sz.width, sz.height);
        m_brush->SetColor(c.surface);
        m_rt->FillRectangle(footerRect, m_brush.Get());
        m_brush->SetColor(c.border);
        m_rt->DrawLine(D2D1::Point2F(0, sz.height - 54.0f), D2D1::Point2F(sz.width, sz.height - 54.0f), m_brush.Get(), 1.0f);

        std::wstring statusText = m_statusBanner;
        if (statusText.empty() && !m_scanning.load()) {
            statusText = tools3000::core::trW(L"扫描耗时 ", L"Scanned in ") +
                         std::to_wstring(snap.elapsedMs) + L" ms · " +
                         std::to_wstring(snap.processes.size()) +
                         tools3000::core::trW(L" 个占用进程", L" locking process(es)");
        }
        drawText(statusText, m_bodyFormat.Get(), D2D1::RectF(18, sz.height - 46, sz.width - 310, sz.height - 8), c.textSecondary);

        if (!snap.currentProcessElevated) {
            drawBtn(D2D1::RectF(sz.width - 296, sz.height - 42, sz.width - 122, sz.height - 12),
                    tools3000::core::trW(L"以管理员重新扫描", L"Rescan as Admin"), 3,
                    c.accent, D2D1::ColorF(0xFFFFFF, 1.0f));
        }
        drawBtn(D2D1::RectF(sz.width - 114, sz.height - 42, sz.width - 18, sz.height - 12),
                tools3000::core::trW(L"重新扫描", L"Rescan"), 2,
                c.surfaceHover, c.textPrimary, 0, L"", m_scanning.load());

        if (m_rt->EndDraw() == D2DERR_RECREATE_TARGET) {
            m_rt.Reset();
            m_brush.Reset();
        }
    }

    void handleClick(float xDip, float yDip) {
        for (const auto& btn : m_buttons) {
            if (btn.disabled) continue;
            if (xDip >= btn.rect.left && xDip <= btn.rect.right &&
                yDip >= btn.rect.top && yDip <= btn.rect.bottom) {
                if (btn.kind == 1) {
                    PostMessageW(m_hwnd, WM_CLOSE, 0, 0);
                } else if (btn.kind == 2) {
                    startScanAsync();
                } else if (btn.kind == 3) {
                    if (tools3000::core::FileLocksmithEngine::launchUnlockWindowProcess(m_targetPath, true)) {
                        PostMessageW(m_hwnd, WM_CLOSE, 0, 0);
                    }
                } else if (btn.kind == 4) {
                    std::string err;
                    if (tools3000::core::FileLocksmithEngine::terminateProcessSafely(btn.pid, err)) {
                        m_statusBanner = tools3000::core::trW(L"[OK] 进程已终止，正在重新验证...", L"[OK] Process terminated, rescanning...");
                        startScanAsync();
                    } else {
                        m_statusBanner = (err == "access_denied")
                            ? tools3000::core::trW(L"[WARN] 权限不足，请点击「以管理员重新扫描」", L"[WARN] Access denied. Please rescan as Administrator.")
                            : tools3000::core::trW(L"[WARN] 无法终止目标进程", L"[WARN] Unable to terminate target process.");
                        InvalidateRect(m_hwnd, nullptr, FALSE);
                    }
                } else if (btn.kind == 5) {
                    std::string err;
                    if (tools3000::core::FileLocksmithEngine::restartExplorerSafely(err)) {
                        m_statusBanner = tools3000::core::trW(L"[OK] 资源管理器已安全重启", L"[OK] File Explorer restarted safely.");
                        startScanAsync();
                    } else {
                        m_statusBanner = tools3000::core::trW(L"[WARN] 重启资源管理器失败", L"[WARN] Failed to restart File Explorer.");
                        InvalidateRect(m_hwnd, nullptr, FALSE);
                    }
                } else if (btn.kind == 6 && !btn.path.empty()) {
                    tools3000::core::WinUtils::openFolderAndSelectItem(btn.path);
                }
                return;
            }
        }
        // 点击顶栏空白处拖动窗口
        if (yDip >= 0.0f && yDip <= 64.0f) {
            ReleaseCapture();
            SendMessageW(m_hwnd, WM_NCLBUTTONDOWN, HTCAPTION, 0);
        }
    }

    static LRESULT CALLBACK wndProc(HWND hwnd, UINT uMsg, WPARAM wParam, LPARAM lParam) {
        FileLocksmithDialog* self = nullptr;
        if (uMsg == WM_NCCREATE) {
            const auto* cs = reinterpret_cast<const CREATESTRUCTW*>(lParam);
            self = static_cast<FileLocksmithDialog*>(cs->lpCreateParams);
            SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
        } else {
            self = reinterpret_cast<FileLocksmithDialog*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
        }

        LRESULT ncRes = 0;
        if (tools3000::core::WinUtils::handleFramelessNonClientMessage(hwnd, uMsg, wParam, lParam, ncRes)) {
            return ncRes;
        }

        if (self) {
            switch (uMsg) {
                case WM_NCCALCSIZE:
                    return 0;
                case WM_ERASEBKGND:
                    return 1;
                case WM_PAINT: {
                    PAINTSTRUCT ps{};
                    BeginPaint(hwnd, &ps);
                    self->paint();
                    EndPaint(hwnd, &ps);
                    return 0;
                }
                case WM_SIZE: {
                    const int w = LOWORD(lParam);
                    const int h = HIWORD(lParam);
                    if (w > 0 && h > 0) {
                        tools3000::core::WinUtils::applyUniversalRoundedCorners(
                            hwnd, w, h, tools3000::core::dpi::scaleMetric(12, self->m_scale));
                    }
                    InvalidateRect(hwnd, nullptr, FALSE);
                    return 0;
                }
                case WM_DPICHANGED: {
                    const unsigned newDpi = HIWORD(wParam);
                    if (newDpi > 0) self->m_scale = tools3000::core::dpi::scaleForDpi(newDpi);
                    if (const auto* rc = reinterpret_cast<const RECT*>(lParam)) {
                        SetWindowPos(hwnd, nullptr, rc->left, rc->top,
                                     rc->right - rc->left, rc->bottom - rc->top,
                                     SWP_NOZORDER | SWP_NOACTIVATE);
                    }
                    InvalidateRect(hwnd, nullptr, FALSE);
                    return 0;
                }
                case WM_MOUSEMOVE: {
                    self->m_mouseDip = D2D1::Point2F(
                        static_cast<float>(GET_X_LPARAM(lParam)) / self->m_scale,
                        static_cast<float>(GET_Y_LPARAM(lParam)) / self->m_scale);
                    InvalidateRect(hwnd, nullptr, FALSE);
                    return 0;
                }
                case WM_LBUTTONDOWN: {
                    const float x = static_cast<float>(GET_X_LPARAM(lParam)) / self->m_scale;
                    const float y = static_cast<float>(GET_Y_LPARAM(lParam)) / self->m_scale;
                    self->handleClick(x, y);
                    return 0;
                }
                case WM_MOUSEWHEEL: {
                    const short delta = GET_WHEEL_DELTA_WPARAM(wParam);
                    self->m_scrollY -= static_cast<float>(delta) * 0.4f;
                    InvalidateRect(hwnd, nullptr, FALSE);
                    return 0;
                }
                case WM_KEYDOWN: {
                    if (wParam == VK_ESCAPE) {
                        PostMessageW(hwnd, WM_CLOSE, 0, 0);
                        return 0;
                    }
                    if (wParam == VK_F5) {
                        self->startScanAsync();
                        return 0;
                    }
                    break;
                }
                case WM_LOCKSMITH_SCAN_DONE:
                    InvalidateRect(hwnd, nullptr, FALSE);
                    return 0;
                case WM_DESTROY:
                    self->m_closing.store(true);
                    PostQuitMessage(0);
                    return 0;
                default:
                    break;
            }
        }
        return DefWindowProcW(hwnd, uMsg, wParam, lParam);
    }

    std::wstring m_targetPath;
    bool m_dark = true;
    float m_scale = 1.0f;
    float m_scrollY = 0.0f;
    D2D1_POINT_2F m_mouseDip{-100.0f, -100.0f};
    HWND m_hwnd = nullptr;

    ComPtr<ID2D1Factory> m_d2dFactory;
    ComPtr<IDWriteFactory> m_dwriteFactory;
    ComPtr<ID2D1HwndRenderTarget> m_rt;
    ComPtr<ID2D1SolidColorBrush> m_brush;
    ComPtr<IDWriteTextFormat> m_titleFormat;
    ComPtr<IDWriteTextFormat> m_bodyFormat;
    ComPtr<IDWriteTextFormat> m_monoFormat;

    std::mutex m_resultMutex;
    tools3000::core::FileLockScanResult m_scanResult;
    std::wstring m_statusBanner;
    std::vector<HitButton> m_buttons;
    std::atomic<bool> m_scanning{false};
    std::atomic<bool> m_closing{false};
    std::thread m_scanThread;
};

}  // namespace

int runFileLocksmithWindow(HINSTANCE hInstance, const std::wstring& targetPath) {
    tools3000::core::WinUtils::enableHighDpiSupport();
    const HRESULT comHr = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
    tools3000::core::ConfigManager::instance().initialize(tools3000::core::WinUtils::getConfigDirectory());
    tools3000::core::UiI18n::setConfiguredLanguage(
        tools3000::core::ConfigManager::instance().get<std::string>("/general/language", "auto"));
    FileLocksmithDialog dlg(targetPath);
    const int code = dlg.run(hInstance ? hInstance : GetModuleHandleW(nullptr));
    tools3000::core::ConfigManager::instance().shutdown();
    if (SUCCEEDED(comHr)) {
        CoUninitialize();
    }
    return code;
}

}  // namespace tools3000::ui
