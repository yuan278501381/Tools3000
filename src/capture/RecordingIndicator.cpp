// ─────────────────────────────────────────────────────────────────────────────
// RecordingIndicator.cpp — 录制状态指示器实现
//
// 外观:
//   ┌──────────────────────────────────────────┐
//   │ 🔴  REC  03:45  1800帧   ⏸  ⏹          │
//   └──────────────────────────────────────────┘
// ─────────────────────────────────────────────────────────────────────────────

#include "capture/RecordingIndicator.h"
#include "capture/CaptureVectorIcons.h"
#include "capture/FloatingGlassBar.h"
#include "capture/ScreenRecorder.h"
#include "core/events/EventBus.h"
#include "core/logger/Logger.h"
#include "core/utils/DpiUtils.h"
#include "core/utils/WinUtils.h"

#include <format>
#include <algorithm>
#include <cmath>
#include <filesystem>
#include <windowsx.h>

namespace tools3000::capture {

using namespace Microsoft::WRL;

static constexpr const wchar_t* INDICATOR_CLASS = L"Tools3000_RecordingIndicator";
static constexpr const wchar_t* BORDER_CLASS = L"Tools3000_RecordingBorder";
static constexpr const wchar_t* COUNTDOWN_CLASS = L"Tools3000_RecordingCountdown";
static bool s_borderClassRegistered = false;
static bool s_countdownClassRegistered = false;
static constexpr UINT_PTR RENDER_TIMER_ID = 3001;
static constexpr int INDICATOR_WIDTH = 486;
static constexpr int INDICATOR_HEIGHT = 38;
static constexpr int BTN_SIZE = 28;

static float indicatorDpiScale(HMONITOR monitor) {
    return tools3000::core::dpi::scaleForMonitor(monitor);
}

static int scaledIndicatorMetric(int value, float scale) {
    return tools3000::core::dpi::scaleMetric(value, scale);
}

RecordingIndicator& RecordingIndicator::instance() {
    static RecordingIndicator inst;
    return inst;
}

bool RecordingIndicator::initialize(HINSTANCE hInstance) {
    // 与截图覆盖层一样按需创建，避免隐藏的 layered D2D 窗口参与桌面合成。
    m_hInstance = hInstance;
    return m_hInstance != nullptr;
}

void RecordingIndicator::setPaused(bool paused) {
    m_paused = paused;
    if (m_borderHwnd) InvalidateRect(m_borderHwnd, nullptr, FALSE);
    if (m_hwnd) InvalidateRect(m_hwnd, nullptr, FALSE);
}

void RecordingIndicator::setRecordingRegion(int x, int y, int width, int height, float cornerRadius) {
    m_recordingRegion = { x, y, x + width, y + height };
    m_recordingCornerRadius = (std::max)(0.0f, cornerRadius);
    m_hasRecordingRegion = (width > 0 && height > 0);
}

void RecordingIndicator::clearRecordingRegion() {
    m_hasRecordingRegion = false;
    m_recordingRegion = {};
    destroyBorderWindow();
    destroyCountdownWindow();
}

void RecordingIndicator::shutdown() {
    if (m_hwnd) KillTimer(m_hwnd, RENDER_TIMER_ID);
    destroyBorderWindow();
    destroyCountdownWindow();
    m_hasRecordingRegion = false;
    releaseRenderResources();
    if (m_hwnd) {
        DestroyWindow(m_hwnd);
        m_hwnd = nullptr;
    }
}

void RecordingIndicator::show() {
    m_completed = false;
    m_completedFilePath.clear();
    m_completedShowTick = 0;

    POINT cursor{};
    GetCursorPos(&cursor);
    if (m_hasRecordingRegion) {
        cursor.x = (m_recordingRegion.left + m_recordingRegion.right) / 2;
        cursor.y = (m_recordingRegion.top + m_recordingRegion.bottom) / 2;
    }
    const HMONITOR monitor = MonitorFromPoint(cursor, MONITOR_DEFAULTTONEAREST);
    m_dpiScale = indicatorDpiScale(monitor);

    if (!m_hwnd) {
        if (!m_hInstance || !createWindow(m_hInstance) || !createRenderResources()) {
            LOG_ERROR("录制指示器按需初始化失败");
            releaseRenderResources();
            if (m_hwnd) {
                DestroyWindow(m_hwnd);
                m_hwnd = nullptr;
            }
            return;
        }
        LOG_DEBUG("录制指示器按需初始化成功");
    }

    // Follow the monitor where region selection finished, including its work
    // area and effective DPI. This also avoids forcing the bar onto display 1.
    MONITORINFO monitorInfo{sizeof(monitorInfo)};
    GetMonitorInfoW(monitor, &monitorInfo);
    const int width = scaledIndicatorMetric(INDICATOR_WIDTH, m_dpiScale);
    const int height = scaledIndicatorMetric(INDICATOR_HEIGHT, m_dpiScale);

    int x = 0;
    int y = 0;
    if (m_hasRecordingRegion) {
        // 智能吸附至录制区域边沿：水平居中，优先吸附在录制区域下方
        int regW = m_recordingRegion.right - m_recordingRegion.left;
        x = m_recordingRegion.left + (regW - width) / 2;
        y = m_recordingRegion.bottom + scaledIndicatorMetric(8, m_dpiScale);
        if (y + height > monitorInfo.rcWork.bottom) {
            // 下方放不下，智能吸附在录制区域上方
            y = m_recordingRegion.top - height - scaledIndicatorMetric(8, m_dpiScale);
        }
        if (y < monitorInfo.rcWork.top) {
            // 上方也放不下，贴合在录制区域内部顶部
            y = m_recordingRegion.top + scaledIndicatorMetric(8, m_dpiScale);
        }
        x = std::clamp(x, static_cast<int>(monitorInfo.rcWork.left),
                          static_cast<int>(monitorInfo.rcWork.right - width));
    } else {
        x = monitorInfo.rcWork.left +
            ((monitorInfo.rcWork.right - monitorInfo.rcWork.left) - width) / 2;
        y = monitorInfo.rcWork.top + scaledIndicatorMetric(8, m_dpiScale);
    }

    SetWindowPos(m_hwnd, HWND_TOPMOST, x, y, width, height,
                 SWP_SHOWWINDOW | SWP_NOACTIVATE);

    // 创建常驻镂空发光呼吸线框
    if (m_hasRecordingRegion && m_hInstance) {
        createBorderWindow(m_hInstance);
    }

    m_blinkTick = GetTickCount();
    SetTimer(m_hwnd, RENDER_TIMER_ID, 100, nullptr);  // 10 Hz 电平刷新；闪烁仍按 500 ms 计算
    LOG_INFO("录制指示器已显示");
}

void RecordingIndicator::hide() {
    destroyBorderWindow();
    destroyCountdownWindow();
    m_hasRecordingRegion = false;
    m_completed = false;
    m_completedFilePath.clear();
    m_completedShowTick = 0;
    if (m_hwnd) {
        KillTimer(m_hwnd, RENDER_TIMER_ID);
        ShowWindow(m_hwnd, SW_HIDE);
        releaseRenderResources();
        DestroyWindow(m_hwnd);
        m_hwnd = nullptr;
    }
    m_paused = false;
    m_countingDown = false;
    m_countdownRemaining = 0;
    m_systemAudioPeak = 0.0f;
    m_microphonePeak = 0.0f;
    m_storageWarning = false;
    m_estimatedRemainingSec = -1;
    m_performanceLimited = false;
    m_effectiveFps = 0.0;
    LOG_DEBUG("录制指示器已隐藏");
}

void RecordingIndicator::showCompleted(const std::string& filePath, double durationSec) {
    destroyBorderWindow();
    destroyCountdownWindow();
    m_hasRecordingRegion = false;
    m_completed = true;
    m_completedFilePath = filePath;
    m_duration = durationSec;
    m_completedShowTick = GetTickCount();

    POINT cursor{};
    GetCursorPos(&cursor);
    const HMONITOR monitor = MonitorFromPoint(cursor, MONITOR_DEFAULTTONEAREST);
    m_dpiScale = indicatorDpiScale(monitor);

    if (!m_hwnd) {
        if (!m_hInstance || !createWindow(m_hInstance) || !createRenderResources()) {
            LOG_ERROR("录制指示器按需初始化失败");
            return;
        }
    }

    MONITORINFO monitorInfo{sizeof(monitorInfo)};
    GetMonitorInfoW(monitor, &monitorInfo);
    const int width = scaledIndicatorMetric(INDICATOR_WIDTH, m_dpiScale);
    const int height = scaledIndicatorMetric(INDICATOR_HEIGHT, m_dpiScale);

    RECT curRect{};
    int x = 0;
    int y = 0;
    if (IsWindowVisible(m_hwnd) && GetWindowRect(m_hwnd, &curRect)) {
        x = curRect.left;
        y = curRect.top;
    } else {
        x = monitorInfo.rcWork.left +
            ((monitorInfo.rcWork.right - monitorInfo.rcWork.left) - width) / 2;
        y = monitorInfo.rcWork.top + scaledIndicatorMetric(16, m_dpiScale);
    }

    SetWindowPos(m_hwnd, HWND_TOPMOST, x, y, width, height,
                 SWP_SHOWWINDOW | SWP_NOACTIVATE);

    if (m_renderTarget) {
        m_renderTarget->Resize(D2D1::SizeU(width, height));
    }

    SetTimer(m_hwnd, RENDER_TIMER_ID, 100, nullptr);
    render();
    LOG_INFO("录制指示器已显示");
}

void RecordingIndicator::update(double durationSec, int frameCount) {
    m_duration = durationSec;
    m_frames = frameCount;
    if (m_hwnd && IsWindowVisible(m_hwnd)) {
        render();
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// 窗口
// ─────────────────────────────────────────────────────────────────────────────

bool RecordingIndicator::createWindow(HINSTANCE hInstance) {
    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.style = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc = indicatorWndProc;
    wc.hInstance = hInstance;
    wc.hCursor = LoadCursor(nullptr, IDC_HAND);
    wc.lpszClassName = INDICATOR_CLASS;
    RegisterClassExW(&wc);

    m_hwnd = CreateWindowExW(
        WS_EX_LAYERED | WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE,
        INDICATOR_CLASS, L"",
        WS_POPUP,
        0, 0, scaledIndicatorMetric(INDICATOR_WIDTH, m_dpiScale),
        scaledIndicatorMetric(INDICATOR_HEIGHT, m_dpiScale),
        nullptr, nullptr, hInstance, this
    );

    if (!m_hwnd) return false;

    SetWindowLongPtrW(m_hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(this));
    SetLayeredWindowAttributes(m_hwnd, 0, 230, LWA_ALPHA);
    if (!tools3000::core::WinUtils::excludeWindowFromCapture(m_hwnd)) {
        LOG_WARN("当前 Windows 版本无法从捕获中排除录制悬浮条: error={}", GetLastError());
    }

    const int btnY = (INDICATOR_HEIGHT - BTN_SIZE) / 2;
    // 录制态按键区域
    m_stopBtn = {INDICATOR_WIDTH - BTN_SIZE - 8, btnY, INDICATOR_WIDTH - 8, btnY + BTN_SIZE};
    m_pauseBtn = {m_stopBtn.left - BTN_SIZE - 8, btnY, m_stopBtn.left - 8, btnY + BTN_SIZE};
    m_snapshotBtn = {m_pauseBtn.left - BTN_SIZE - 8, btnY, m_pauseBtn.left - 8, btnY + BTN_SIZE};
    m_microphoneBtn = {m_snapshotBtn.left - BTN_SIZE - 12, btnY, m_snapshotBtn.left - 12, btnY + BTN_SIZE};
    m_systemAudioBtn = {m_microphoneBtn.left - BTN_SIZE - 8, btnY, m_microphoneBtn.left - 8, btnY + BTN_SIZE};

    // 完成态按键区域
    m_closeBtn = {INDICATOR_WIDTH - BTN_SIZE - 8, btnY, INDICATOR_WIDTH - 8, btnY + BTN_SIZE};
    m_copyFileBtn = {m_closeBtn.left - BTN_SIZE - 8, btnY, m_closeBtn.left - 8, btnY + BTN_SIZE};
    m_openFolderBtn = {m_copyFileBtn.left - BTN_SIZE - 8, btnY, m_copyFileBtn.left - 8, btnY + BTN_SIZE};

    return true;
}

bool RecordingIndicator::createRenderResources() {
    HRESULT hr;

    hr = D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, m_d2dFactory.GetAddressOf());
    if (FAILED(hr)) return false;

    hr = DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory),
                             reinterpret_cast<IUnknown**>(m_dwriteFactory.GetAddressOf()));
    if (FAILED(hr)) return false;

    m_dwriteFactory->CreateTextFormat(L"Segoe UI", nullptr,
                                       DWRITE_FONT_WEIGHT_SEMI_BOLD, DWRITE_FONT_STYLE_NORMAL,
                                       DWRITE_FONT_STRETCH_NORMAL, 13.0f, L"zh-CN",
                                       m_textFormat.GetAddressOf());

    m_dwriteFactory->CreateTextFormat(L"Segoe UI Symbol", nullptr,
                                       DWRITE_FONT_WEIGHT_NORMAL, DWRITE_FONT_STYLE_NORMAL,
                                       DWRITE_FONT_STRETCH_NORMAL, 16.0f, L"zh-CN",
                                       m_btnTextFormat.GetAddressOf());

    RECT rc;
    GetClientRect(m_hwnd, &rc);

    auto rtProps = D2D1::RenderTargetProperties(
        D2D1_RENDER_TARGET_TYPE_DEFAULT,
        D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED)
    );
    auto hwndProps = D2D1::HwndRenderTargetProperties(
        m_hwnd, D2D1::SizeU(rc.right, rc.bottom), D2D1_PRESENT_OPTIONS_IMMEDIATELY
    );

    hr = m_d2dFactory->CreateHwndRenderTarget(rtProps, hwndProps, m_renderTarget.GetAddressOf());
    if (FAILED(hr)) return false;

    // 禁用 D2D 的自动 DPI 缩放
    m_renderTarget->SetDpi(96.0f, 96.0f);

    m_renderTarget->CreateSolidColorBrush(D2D1::ColorF(0.12f, 0.12f, 0.15f, 1.0f), m_bgBrush.GetAddressOf());
    m_renderTarget->CreateSolidColorBrush(D2D1::ColorF(1, 1, 1, 0.95f), m_textBrush.GetAddressOf());
    m_renderTarget->CreateSolidColorBrush(D2D1::ColorF(0.95f, 0.2f, 0.2f, 1.0f), m_redDotBrush.GetAddressOf());
    m_renderTarget->CreateSolidColorBrush(D2D1::ColorF(0.95f, 0.2f, 0.2f, 0.28f), m_recordingAuraBrush.GetAddressOf());
    m_renderTarget->CreateSolidColorBrush(D2D1::ColorF(0.96f, 0.62f, 0.04f, 0.28f), m_pausedAuraBrush.GetAddressOf());
    m_renderTarget->CreateSolidColorBrush(D2D1::ColorF(0.96f, 0.62f, 0.04f, 1.0f), m_pausedDotBrush.GetAddressOf());
    m_renderTarget->CreateSolidColorBrush(D2D1::ColorF(1, 1, 1, 0.15f), m_btnBrush.GetAddressOf());
    m_renderTarget->CreateSolidColorBrush(D2D1::ColorF(1, 1, 1, 0.3f), m_btnHoverBrush.GetAddressOf());
    m_renderTarget->CreateSolidColorBrush(D2D1::ColorF(0.2f, 0.85f, 0.55f, 1.0f), m_audioBrush.GetAddressOf());

    return true;
}

void RecordingIndicator::releaseRenderResources() {
    m_audioBrush.Reset();
    m_btnHoverBrush.Reset();
    m_btnBrush.Reset();
    m_pausedDotBrush.Reset();
    m_pausedAuraBrush.Reset();
    m_recordingAuraBrush.Reset();
    m_redDotBrush.Reset();
    m_textBrush.Reset();
    m_bgBrush.Reset();
    m_btnTextFormat.Reset();
    m_textFormat.Reset();
    m_renderTarget.Reset();
    m_dwriteFactory.Reset();
    m_d2dFactory.Reset();
}

// ─────────────────────────────────────────────────────────────────────────────
// 渲染
// ─────────────────────────────────────────────────────────────────────────────

void RecordingIndicator::render() {
    if (!m_renderTarget) return;

    m_renderTarget->BeginDraw();
    m_renderTarget->SetTransform(D2D1::Matrix3x2F::Scale(m_dpiScale, m_dpiScale));

    const bool isDark = tools3000::core::WinUtils::isSystemDarkMode();
    if (m_textBrush) {
        m_textBrush->SetColor(isDark ? D2D1::ColorF(0.96f, 0.96f, 0.96f, 0.95f)
                                     : D2D1::ColorF(0.12f, 0.12f, 0.14f, 0.95f));
    }

    auto physicalSize = m_renderTarget->GetSize();
    auto size = D2D1::SizeF(physicalSize.width / m_dpiScale,
                            physicalSize.height / m_dpiScale);

    // 绘制通用微晶玻璃浮岛底板 (含多层微阴影、黑曜石/水晶白主体、1px 镜面顶光及外描边)
    FloatingGlassBar::drawGlassPanel(m_renderTarget.Get(),
                                    D2D1::RectF(0, 0, size.width, size.height),
                                    8.0f, isDark);

    if (m_completed) {
        // 完成态：翡翠绿指示圆点 + 柔光辐射
        ComPtr<ID2D1SolidColorBrush> doneAuraBrush, doneDotBrush;
        m_renderTarget->CreateSolidColorBrush(D2D1::ColorF(0.06f, 0.72f, 0.44f, 0.28f), doneAuraBrush.GetAddressOf());
        m_renderTarget->CreateSolidColorBrush(D2D1::ColorF(0.06f, 0.72f, 0.44f, 1.0f), doneDotBrush.GetAddressOf());
        if (doneAuraBrush) {
            m_renderTarget->FillEllipse(D2D1::Ellipse(D2D1::Point2F(18, size.height / 2), 8.5f, 8.5f), doneAuraBrush.Get());
        }
        if (doneDotBrush) {
            m_renderTarget->FillEllipse(D2D1::Ellipse(D2D1::Point2F(18, size.height / 2), 4.5f, 4.5f), doneDotBrush.Get());
        }

        const bool isZh = tools3000::core::WinUtils::isSystemLanguageChinese();
        std::wstring statusText = isZh ? L"已保存" : L"SAVED";
        m_renderTarget->DrawText(statusText.c_str(), static_cast<UINT32>(statusText.size()),
                                 m_textFormat.Get(),
                                 D2D1::RectF(32, 0, 84, size.height),
                                 m_textBrush.Get());

        auto timeStr = formatDuration(m_duration);
        m_renderTarget->DrawText(timeStr.c_str(), static_cast<UINT32>(timeStr.size()),
                                 m_textFormat.Get(),
                                 D2D1::RectF(84, 0, 154, size.height),
                                 m_textBrush.Get());

        // 文件名或状态提示
        std::wstring fileName;
        if (!m_completedFilePath.empty()) {
            std::filesystem::path p(tools3000::core::WinUtils::utf8ToWstring(m_completedFilePath));
            fileName = p.filename().wstring();
        }
        if (fileName.empty()) {
            fileName = isZh ? L"录屏文件就绪" : L"Video Ready";
        }
        m_renderTarget->DrawText(fileName.c_str(), static_cast<UINT32>(fileName.size()),
                                 m_textFormat.Get(),
                                 D2D1::RectF(156, 0, 368, size.height),
                                 m_textBrush.Get());

        // 分隔线
        FloatingGlassBar::drawSeparator(m_renderTarget.Get(), 372.0f, 6.0f, size.height - 6.0f, 1.0f, isDark);

        // 按钮1: 打开所在文件夹
        D2D1_RECT_F ofRc = D2D1::RectF(static_cast<float>(m_openFolderBtn.left), static_cast<float>(m_openFolderBtn.top),
                                       static_cast<float>(m_openFolderBtn.right), static_cast<float>(m_openFolderBtn.bottom));
        FloatingGlassBar::drawButtonPill(m_renderTarget.Get(), ofRc, 5.0f, m_hoverOpenFolder, false, false, false, 1.0f, isDark);
        D2D1_RECT_F ofIconRc = D2D1::RectF(ofRc.left + 4, ofRc.top + 4, ofRc.right - 4, ofRc.bottom - 4);
        CaptureVectorIcons::renderIcon(m_renderTarget.Get(), m_d2dFactory.Get(), CaptureIconId::ActionOpenFolder, ofIconRc, m_textBrush.Get(), 1.0f);

        // 按钮2: 复制文件到剪贴板
        D2D1_RECT_F cfRc = D2D1::RectF(static_cast<float>(m_copyFileBtn.left), static_cast<float>(m_copyFileBtn.top),
                                       static_cast<float>(m_copyFileBtn.right), static_cast<float>(m_copyFileBtn.bottom));
        FloatingGlassBar::drawButtonPill(m_renderTarget.Get(), cfRc, 5.0f, m_hoverCopyFile, false, false, false, 1.0f, isDark);
        D2D1_RECT_F cfIconRc = D2D1::RectF(cfRc.left + 4, cfRc.top + 4, cfRc.right - 4, cfRc.bottom - 4);
        CaptureVectorIcons::renderIcon(m_renderTarget.Get(), m_d2dFactory.Get(), CaptureIconId::ActionCopy, cfIconRc, m_textBrush.Get(), 1.0f);

        // 按钮3: 关闭浮岛卡片
        D2D1_RECT_F clRc = D2D1::RectF(static_cast<float>(m_closeBtn.left), static_cast<float>(m_closeBtn.top),
                                       static_cast<float>(m_closeBtn.right), static_cast<float>(m_closeBtn.bottom));
        FloatingGlassBar::drawButtonPill(m_renderTarget.Get(), clRc, 5.0f, m_hoverClose, false, false, false, 1.0f, isDark);
        D2D1_RECT_F clIconRc = D2D1::RectF(clRc.left + 5, clRc.top + 5, clRc.right - 5, clRc.bottom - 5);
        CaptureVectorIcons::renderIcon(m_renderTarget.Get(), m_d2dFactory.Get(), CaptureIconId::ActionCancel, clIconRc, m_textBrush.Get(), 1.0f);
    } else {
        // 录制状态指示点（辐射发光光晕 + 核心指示点；暂停时黄光呼吸，录制时红光）
        bool showDot = !m_paused || ((GetTickCount() - m_blinkTick) / 500 % 2 == 0);
        if (showDot) {
            auto* auraBrush = m_paused ? m_pausedAuraBrush.Get() : m_recordingAuraBrush.Get();
            auto* dotBrush = m_paused ? m_pausedDotBrush.Get() : m_redDotBrush.Get();
            if (auraBrush) {
                m_renderTarget->FillEllipse(
                    D2D1::Ellipse(D2D1::Point2F(18, size.height / 2), 8.5f, 8.5f),
                    auraBrush
                );
            }
            if (dotBrush) {
                m_renderTarget->FillEllipse(
                    D2D1::Ellipse(D2D1::Point2F(18, size.height / 2), 4.5f, 4.5f),
                    dotBrush
                );
            }
        }

        // REC 文字
        std::wstring recText = m_countingDown ? L"READY" : (m_paused ? L"PAUSED" : L"REC");
        m_renderTarget->DrawText(recText.c_str(), static_cast<UINT32>(recText.size()),
                                 m_textFormat.Get(),
                                 D2D1::RectF(32, 0, 84, size.height),
                                 m_textBrush.Get());

        // 时间 (HH:MM:SS)
        auto timeStr = m_countingDown
            ? std::to_wstring(std::max(1, m_countdownRemaining))
            : formatDuration(m_duration);
        m_renderTarget->DrawText(timeStr.c_str(), static_cast<UINT32>(timeStr.size()),
                                 m_textFormat.Get(),
                                 D2D1::RectF(84, 0, 154, size.height),
                                 m_textBrush.Get());

        // 实时分辨率 (如 1920×1080)
        std::wstring resStr;
        if (m_hasRecordingRegion) {
            int rw = m_recordingRegion.right - m_recordingRegion.left;
            int rh = m_recordingRegion.bottom - m_recordingRegion.top;
            resStr = std::format(L"{}×{}", rw, rh);
        } else {
            resStr = std::format(L"{}×{}", GetSystemMetrics(SM_CXSCREEN), GetSystemMetrics(SM_CYSCREEN));
        }
        m_renderTarget->DrawText(resStr.c_str(), static_cast<UINT32>(resStr.size()),
                                 m_textFormat.Get(),
                                 D2D1::RectF(156, 0, 226, size.height),
                                 m_textBrush.Get());

        // 帧数 / 性能状态
        auto frameStr = m_countingDown ? std::wstring{}
            : (m_storageWarning && m_estimatedRemainingSec >= 0
                ? std::format(L"~{}m", std::max<std::int64_t>(1, m_estimatedRemainingSec / 60))
                : m_performanceLimited
                    ? std::format(L"{:.0f}fps", m_effectiveFps)
                    : std::format(L"{}帧", m_frames));
        m_renderTarget->DrawText(frameStr.c_str(), static_cast<UINT32>(frameStr.size()),
                                 m_textFormat.Get(),
                                 D2D1::RectF(228, 0, 298, size.height),
                                 m_textBrush.Get());

        // 系统声/麦克风双通道实时电平
        const auto drawMeter = [&](const RECT& rect, CaptureIconId iconId,
                                   float peak, bool active, bool muted, bool hovered) {
            if (!active) return;
            D2D1_RECT_F bounds = D2D1::RectF(static_cast<float>(rect.left), static_cast<float>(rect.top),
                                             static_cast<float>(rect.right), static_cast<float>(rect.bottom));
            FloatingGlassBar::drawButtonPill(m_renderTarget.Get(), bounds, 5.0f, hovered, muted, muted, false, 1.0f, isDark);

            // 纯矢量麦克风/扬声器图标
            D2D1_RECT_F iconRect = D2D1::RectF(
                static_cast<float>(rect.left + 2), static_cast<float>(rect.top + 2),
                static_cast<float>(rect.right - 8), static_cast<float>(rect.bottom - 2));
            CaptureVectorIcons::renderIcon(m_renderTarget.Get(), m_d2dFactory.Get(), iconId, iconRect, m_textBrush.Get(), 1.0f);

            const float normalized = std::clamp(peak, 0.0f, 1.0f);
            if (!muted && normalized > 0.005f) {
                const auto fill = D2D1::RoundedRect(
                    D2D1::RectF(static_cast<float>(rect.right - 5),
                                 static_cast<float>(rect.bottom - 3) - normalized * 22,
                                 static_cast<float>(rect.right - 2),
                                 static_cast<float>(rect.bottom - 3)), 2.0f, 2.0f);
                m_renderTarget->FillRoundedRectangle(fill, m_audioBrush.Get());
            }
        };
        if (!m_countingDown) {
            drawMeter(m_systemAudioBtn, CaptureIconId::ActionToggleSpeaker, m_systemAudioPeak,
                      m_systemAudioActive, m_systemAudioMuted, m_hoverSysAudio);
            drawMeter(m_microphoneBtn, CaptureIconId::ActionToggleMic, m_microphonePeak,
                      m_microphoneActive, m_microphoneMuted, m_hoverMic);
        }

        // 分隔线
        FloatingGlassBar::drawSeparator(m_renderTarget.Get(), 372.0f, 6.0f, size.height - 6.0f, 1.0f, isDark);

        // 快照按钮 (ActionSnapshot 相机图标)
        if (!m_countingDown) {
            D2D1_RECT_F snapRc = D2D1::RectF(static_cast<float>(m_snapshotBtn.left), static_cast<float>(m_snapshotBtn.top),
                                             static_cast<float>(m_snapshotBtn.right), static_cast<float>(m_snapshotBtn.bottom));
            FloatingGlassBar::drawButtonPill(m_renderTarget.Get(), snapRc, 5.0f, m_hoverSnapshot, false, false, false, 1.0f, isDark);
            D2D1_RECT_F snapIconRc = D2D1::RectF(snapRc.left + 4, snapRc.top + 4, snapRc.right - 4, snapRc.bottom - 4);
            CaptureVectorIcons::renderIcon(m_renderTarget.Get(), m_d2dFactory.Get(), CaptureIconId::ActionSnapshot, snapIconRc, m_textBrush.Get(), 1.0f);
        }

        // 倒计时只能取消，开始写帧后才显示暂停按钮。
        if (!m_countingDown) {
            D2D1_ROUNDED_RECT pauseRect = D2D1::RoundedRect(
                D2D1::RectF(static_cast<float>(m_pauseBtn.left), static_cast<float>(m_pauseBtn.top),
                             static_cast<float>(m_pauseBtn.right), static_cast<float>(m_pauseBtn.bottom)),
                5.0f, 5.0f
            );
            D2D1_RECT_F pauseRc = D2D1::RectF(static_cast<float>(m_pauseBtn.left), static_cast<float>(m_pauseBtn.top),
                                              static_cast<float>(m_pauseBtn.right), static_cast<float>(m_pauseBtn.bottom));
            FloatingGlassBar::drawButtonPill(m_renderTarget.Get(), pauseRc, 5.0f, m_hoverPause, m_paused, false, false, 1.0f, isDark);
            D2D1_RECT_F pIconRc = D2D1::RectF(
                static_cast<float>(m_pauseBtn.left + 4), static_cast<float>(m_pauseBtn.top + 4),
                static_cast<float>(m_pauseBtn.right - 4), static_cast<float>(m_pauseBtn.bottom - 4));
            CaptureVectorIcons::renderIcon(m_renderTarget.Get(), m_d2dFactory.Get(),
                m_paused ? CaptureIconId::ActionRecordStart : CaptureIconId::ActionRecordPause,
                pIconRc, m_textBrush.Get(), 1.0f);
        }

        // 停止按钮 (柔和危险正红药丸 + 停止图标)
        D2D1_RECT_F stopRc = D2D1::RectF(static_cast<float>(m_stopBtn.left), static_cast<float>(m_stopBtn.top),
                                         static_cast<float>(m_stopBtn.right), static_cast<float>(m_stopBtn.bottom));
        FloatingGlassBar::drawButtonPill(m_renderTarget.Get(), stopRc, 5.0f, m_hoverStop, false, true, false, 1.0f, isDark);
        D2D1_RECT_F sIconRc = D2D1::RectF(
            static_cast<float>(m_stopBtn.left + 4), static_cast<float>(m_stopBtn.top + 4),
            static_cast<float>(m_stopBtn.right - 4), static_cast<float>(m_stopBtn.bottom - 4));
        CaptureVectorIcons::renderIcon(m_renderTarget.Get(), m_d2dFactory.Get(),
            CaptureIconId::ActionRecordStop, sIconRc, m_textBrush.Get(), 1.0f);
    }

    const HRESULT hrEnd = m_renderTarget->EndDraw();
    if (hrEnd == D2DERR_RECREATE_TARGET) {
        releaseRenderResources();
        createRenderResources();
    }
}

std::wstring RecordingIndicator::formatDuration(double seconds) const {
    int totalSec = static_cast<int>(seconds);
    int hour = totalSec / 3600;
    int min = (totalSec % 3600) / 60;
    int sec = totalSec % 60;
    return std::format(L"{:02d}:{:02d}:{:02d}", hour, min, sec);
}

// ─────────────────────────────────────────────────────────────────────────────
// 窗口过程
// ─────────────────────────────────────────────────────────────────────────────

LRESULT CALLBACK RecordingIndicator::indicatorWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    auto* self = reinterpret_cast<RecordingIndicator*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));

    switch (msg) {
        case WM_LBUTTONDOWN: {
            if (!self) break;
            POINT pt = {GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
            POINT hitPt = {
                static_cast<LONG>(pt.x / self->m_dpiScale),
                static_cast<LONG>(pt.y / self->m_dpiScale)};

            if (self->m_completed) {
                if (PtInRect(&self->m_openFolderBtn, hitPt)) {
                    tools3000::core::WinUtils::showInExplorer(tools3000::core::WinUtils::utf8ToWstring(self->m_completedFilePath));
                    return 0;
                }
                if (PtInRect(&self->m_copyFileBtn, hitPt)) {
                    bool isGif = false;
                    if (!self->m_completedFilePath.empty()) {
                        std::filesystem::path p(tools3000::core::WinUtils::utf8ToWstring(self->m_completedFilePath));
                        auto ext = p.extension().string();
                        std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
                        isGif = (ext == ".gif");
                    }
                    const bool isZh = tools3000::core::WinUtils::isSystemLanguageChinese();
                    if (isGif) {
                        tools3000::core::WinUtils::copyGifToClipboard(tools3000::core::WinUtils::utf8ToWstring(self->m_completedFilePath));
                        tools3000::core::EventBus::instance().publish(
                            tools3000::core::ShowToastEvent{isZh ? L"✓ 已复制 GIF，支持在微信中直接粘贴播放" : L"✓ GIF copied, ready to paste & play in WeChat"});
                    } else {
                        tools3000::core::WinUtils::copyFileToClipboard(tools3000::core::WinUtils::utf8ToWstring(self->m_completedFilePath));
                        tools3000::core::EventBus::instance().publish(
                            tools3000::core::ShowToastEvent{isZh ? L"✓ 录屏文件已复制到剪贴板" : L"✓ Recording file copied to clipboard"});
                    }
                    return 0;
                }
                if (PtInRect(&self->m_closeBtn, hitPt)) {
                    self->hide();
                    return 0;
                }
                self->m_isDragging = true;
                self->m_dragOffset = pt;
                SetCapture(hwnd);
                return 0;
            }

            // 检查是否点击了按钮
            if (!self->m_countingDown && PtInRect(&self->m_pauseBtn, hitPt)) {
                if (self->m_onPause) self->m_onPause();
                return 0;
            }
            if (PtInRect(&self->m_stopBtn, hitPt)) {
                if (self->m_onStop) self->m_onStop();
                return 0;
            }
            if (!self->m_countingDown && PtInRect(&self->m_snapshotBtn, hitPt)) {
                if (self->m_onSnapshot) self->m_onSnapshot();
                return 0;
            }
            if (self->m_systemAudioActive && PtInRect(&self->m_systemAudioBtn, hitPt)) {
                if (self->m_onSystemAudioMute) self->m_onSystemAudioMute();
                return 0;
            }
            if (self->m_microphoneActive && PtInRect(&self->m_microphoneBtn, hitPt)) {
                if (self->m_onMicrophoneMute) self->m_onMicrophoneMute();
                return 0;
            }

            // 拖拽移动
            self->m_isDragging = true;
            self->m_dragOffset = pt;
            SetCapture(hwnd);
            return 0;
        }

        case WM_MOUSEMOVE: {
            if (!self) break;
            POINT pt = {GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
            if (self->m_isDragging) {
                POINT cursor;
                GetCursorPos(&cursor);
                SetWindowPos(hwnd, nullptr,
                             cursor.x - self->m_dragOffset.x,
                             cursor.y - self->m_dragOffset.y,
                             0, 0, SWP_NOSIZE | SWP_NOZORDER);
                return 0;
            }

            TRACKMOUSEEVENT tme{sizeof(tme)};
            tme.dwFlags = TME_LEAVE;
            tme.hwndTrack = hwnd;
            TrackMouseEvent(&tme);

            POINT hitPt = {
                static_cast<LONG>(pt.x / self->m_dpiScale),
                static_cast<LONG>(pt.y / self->m_dpiScale)};

            bool changed = false;
            if (self->m_completed) {
                bool hOpen = PtInRect(&self->m_openFolderBtn, hitPt);
                bool hCopy = PtInRect(&self->m_copyFileBtn, hitPt);
                bool hClose = PtInRect(&self->m_closeBtn, hitPt);
                if (hOpen != self->m_hoverOpenFolder || hCopy != self->m_hoverCopyFile || hClose != self->m_hoverClose) {
                    self->m_hoverOpenFolder = hOpen;
                    self->m_hoverCopyFile = hCopy;
                    self->m_hoverClose = hClose;
                    changed = true;
                }
            } else {
                bool hStop = PtInRect(&self->m_stopBtn, hitPt);
                bool hPause = !self->m_countingDown && PtInRect(&self->m_pauseBtn, hitPt);
                bool hSnap = !self->m_countingDown && PtInRect(&self->m_snapshotBtn, hitPt);
                bool hSys = self->m_systemAudioActive && PtInRect(&self->m_systemAudioBtn, hitPt);
                bool hMic = self->m_microphoneActive && PtInRect(&self->m_microphoneBtn, hitPt);
                if (hStop != self->m_hoverStop || hPause != self->m_hoverPause ||
                    hSnap != self->m_hoverSnapshot || hSys != self->m_hoverSysAudio ||
                    hMic != self->m_hoverMic) {
                    self->m_hoverStop = hStop;
                    self->m_hoverPause = hPause;
                    self->m_hoverSnapshot = hSnap;
                    self->m_hoverSysAudio = hSys;
                    self->m_hoverMic = hMic;
                    changed = true;
                }
            }
            if (changed) {
                self->render();
            }
            return 0;
        }

        case WM_MOUSELEAVE: {
            if (!self) break;
            bool changed = self->m_hoverStop || self->m_hoverPause || self->m_hoverSnapshot ||
                           self->m_hoverSysAudio || self->m_hoverMic || self->m_hoverOpenFolder ||
                           self->m_hoverCopyFile || self->m_hoverClose;
            self->m_hoverStop = false;
            self->m_hoverPause = false;
            self->m_hoverSnapshot = false;
            self->m_hoverSysAudio = false;
            self->m_hoverMic = false;
            self->m_hoverOpenFolder = false;
            self->m_hoverCopyFile = false;
            self->m_hoverClose = false;
            if (changed) self->render();
            return 0;
        }

        case WM_SETCURSOR: {
            if (self && LOWORD(lParam) == HTCLIENT) {
                POINT pt;
                GetCursorPos(&pt);
                ScreenToClient(hwnd, &pt);
                POINT hitPt = {
                    static_cast<LONG>(pt.x / self->m_dpiScale),
                    static_cast<LONG>(pt.y / self->m_dpiScale)};
                bool overBtn = false;
                if (self->m_completed) {
                    overBtn = PtInRect(&self->m_openFolderBtn, hitPt) ||
                              PtInRect(&self->m_copyFileBtn, hitPt) ||
                              PtInRect(&self->m_closeBtn, hitPt);
                } else {
                    overBtn = PtInRect(&self->m_stopBtn, hitPt) ||
                              (!self->m_countingDown && PtInRect(&self->m_pauseBtn, hitPt)) ||
                              (!self->m_countingDown && PtInRect(&self->m_snapshotBtn, hitPt)) ||
                              (self->m_systemAudioActive && PtInRect(&self->m_systemAudioBtn, hitPt)) ||
                              (self->m_microphoneActive && PtInRect(&self->m_microphoneBtn, hitPt));
                }
                SetCursor(LoadCursor(nullptr, overBtn ? IDC_HAND : IDC_SIZEALL));
                return TRUE;
            }
            return DefWindowProcW(hwnd, msg, wParam, lParam);
        }

        case WM_LBUTTONUP: {
            if (self) {
                self->m_isDragging = false;
                ReleaseCapture();
            }
            return 0;
        }

        case WM_TIMER: {
            if (self && wParam == RENDER_TIMER_ID) {
                if (self->m_completed) {
                    if (GetTickCount() - self->m_completedShowTick > 8000) {
                        self->hide();
                        return 0;
                    }
                    self->render();
                    return 0;
                }
                const auto stats = ScreenRecorder::instance().stats();
                self->m_duration = stats.durationSec;
                self->m_frames = stats.frameCount;
                self->m_systemAudioPeak = stats.systemAudioPeak;
                self->m_microphonePeak = stats.microphonePeak;
                self->m_systemAudioActive = stats.systemAudioActive;
                self->m_microphoneActive = stats.microphoneActive;
                self->m_systemAudioMuted = stats.systemAudioMuted;
                self->m_microphoneMuted = stats.microphoneMuted;
                self->m_countingDown = ScreenRecorder::instance().state() == RecordState::Countdown;
                self->m_countdownRemaining = stats.countdownRemaining;
                self->m_storageWarning = stats.storageWarning;
                self->m_estimatedRemainingSec = stats.estimatedRemainingSec;
                self->m_performanceLimited = stats.performanceLimited;
                self->m_effectiveFps = stats.effectiveFps;
                self->updateCountdownWindow();
                self->updateBorderPulse();
                self->render();  // 刷新闪烁
            }
            return 0;
        }

        case WM_DPICHANGED: {
            if (!self) break;
            self->m_dpiScale = std::clamp(
                static_cast<float>(LOWORD(wParam)) / 96.0f, 1.0f, 5.0f);
            const auto* suggested = reinterpret_cast<const RECT*>(lParam);
            const int width = scaledIndicatorMetric(INDICATOR_WIDTH, self->m_dpiScale);
            const int height = scaledIndicatorMetric(INDICATOR_HEIGHT, self->m_dpiScale);
            SetWindowPos(hwnd, nullptr, suggested->left, suggested->top, width, height,
                         SWP_NOZORDER | SWP_NOACTIVATE);
            if (self->m_renderTarget) {
                self->m_renderTarget->Resize(D2D1::SizeU(width, height));
                self->render();
            }
            return 0;
        }

        default:
            return DefWindowProcW(hwnd, msg, wParam, lParam);
    }

    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

// ─────────────────────────────────────────────────────────────────────────────
// 常驻镂空发光呼吸线框实现
// ─────────────────────────────────────────────────────────────────────────────

LRESULT CALLBACK RecordingIndicator::borderWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    if (msg == WM_NCHITTEST) return HTTRANSPARENT;
    if (msg == WM_ERASEBKGND) return 1;
    if (msg == WM_PAINT) {
        PAINTSTRUCT ps;
        HDC hdc = BeginPaint(hwnd, &ps);
        RECT rc;
        GetClientRect(hwnd, &rc);
        bool paused = RecordingIndicator::instance().isPaused();
        // 暂停时黄光呼吸 (RGB(245, 158, 11))、录制中红光提示 (RGB(239, 68, 68))
        COLORREF color = paused ? RGB(245, 158, 11) : RGB(239, 68, 68);
        HBRUSH brush = CreateSolidBrush(color);
        FillRect(hdc, &rc, brush);
        DeleteObject(brush);
        EndPaint(hwnd, &ps);
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

bool RecordingIndicator::createBorderWindow(HINSTANCE hInstance) {
    if (m_borderHwnd) destroyBorderWindow();
    if (!m_hasRecordingRegion) return false;

    if (!s_borderClassRegistered) {
        WNDCLASSEXW wc{};
        wc.cbSize = sizeof(wc);
        wc.style = CS_HREDRAW | CS_VREDRAW;
        wc.lpfnWndProc = borderWndProc;
        wc.hInstance = hInstance;
        wc.lpszClassName = BORDER_CLASS;
        RegisterClassExW(&wc);
        s_borderClassRegistered = true;
    }

    int bw = 3; // 3px 纯正发光边框
    int x = m_recordingRegion.left - bw;
    int y = m_recordingRegion.top - bw;
    int w = (m_recordingRegion.right - m_recordingRegion.left) + bw * 2;
    int h = (m_recordingRegion.bottom - m_recordingRegion.top) + bw * 2;

    m_borderHwnd = CreateWindowExW(
        WS_EX_LAYERED | WS_EX_TRANSPARENT | WS_EX_TOOLWINDOW | WS_EX_TOPMOST | WS_EX_NOACTIVATE,
        BORDER_CLASS, L"",
        WS_POPUP,
        x, y, w, h,
        nullptr, nullptr, hInstance, nullptr
    );

    if (!m_borderHwnd) return false;
    SetPropW(m_borderHwnd, L"NonRudeHWND", reinterpret_cast<HANDLE>(TRUE));

    // 镂空内部区域，仅保留四周边缘，杜绝遮挡中间内容与光标操作
    HRGN outerRgn = nullptr;
    HRGN innerRgn = nullptr;
    if (m_recordingCornerRadius > 0.5f) {
        int outerR = static_cast<int>(std::round(m_recordingCornerRadius + bw));
        int innerR = static_cast<int>(std::round(m_recordingCornerRadius));
        outerRgn = CreateRoundRectRgn(0, 0, w + 1, h + 1, outerR * 2, outerR * 2);
        innerRgn = CreateRoundRectRgn(bw, bw, w - bw + 1, h - bw + 1, innerR * 2, innerR * 2);
    } else {
        outerRgn = CreateRectRgn(0, 0, w, h);
        innerRgn = CreateRectRgn(bw, bw, w - bw, h - bw);
    }
    CombineRgn(outerRgn, outerRgn, innerRgn, RGN_DIFF);
    SetWindowRgn(m_borderHwnd, outerRgn, TRUE);
    DeleteObject(innerRgn);

    SetLayeredWindowAttributes(m_borderHwnd, 0, 220, LWA_ALPHA);

    // 必须从录屏捕获中彻底剔除此发光线框窗口，防止录出的成片包含红框
    if (!tools3000::core::WinUtils::excludeWindowFromCapture(m_borderHwnd)) {
        LOG_WARN("无法从屏幕录制中排除呼吸发光线框: error={}", GetLastError());
    }

    ShowWindow(m_borderHwnd, SW_SHOWNOACTIVATE);
    UpdateWindow(m_borderHwnd);
    return true;
}

void RecordingIndicator::destroyBorderWindow() {
    if (m_borderHwnd) {
        ShowWindow(m_borderHwnd, SW_HIDE);
        DestroyWindow(m_borderHwnd);
        m_borderHwnd = nullptr;
    }
}

void RecordingIndicator::updateBorderPulse() {
    if (!m_borderHwnd || !IsWindow(m_borderHwnd)) return;
    DWORD now = GetTickCount();
    // 1.2 秒一个呼吸周期 (150 ~ 240 alpha)
    float phase = static_cast<float>((now % 1200)) / 1200.0f;
    float pulse = 0.5f + 0.5f * std::sin(phase * 6.2831853f);
    BYTE alpha = static_cast<BYTE>(150.0f + pulse * 90.0f);
    SetLayeredWindowAttributes(m_borderHwnd, 0, alpha, LWA_ALPHA);
}

// ─────────────────────────────────────────────────────────────────────────────
// 视口居中 3-2-1 倒计时微动效窗口实现
// ─────────────────────────────────────────────────────────────────────────────

LRESULT CALLBACK RecordingIndicator::countdownWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    if (msg == WM_NCHITTEST) return HTTRANSPARENT;
    if (msg == WM_KEYDOWN && wParam == VK_SPACE) {
        ScreenRecorder::instance().skipCountdown();
        return 0;
    }
    if (msg == WM_PAINT) {
        PAINTSTRUCT ps;
        HDC hdc = BeginPaint(hwnd, &ps);
        RECT rc;
        GetClientRect(hwnd, &rc);

        int count = ScreenRecorder::instance().stats().countdownRemaining;
        if (count <= 0) count = 1;

        HDC memDC = CreateCompatibleDC(hdc);
        HBITMAP memBm = CreateCompatibleBitmap(hdc, rc.right, rc.bottom);
        HBITMAP oldBm = (HBITMAP)SelectObject(memDC, memBm);

        HBRUSH bgBrush = CreateSolidBrush(RGB(18, 20, 26));
        FillRect(memDC, &rc, bgBrush);
        DeleteObject(bgBrush);

        SetBkMode(memDC, TRANSPARENT);
        SetTextColor(memDC, RGB(255, 255, 255));

        HFONT numFont = CreateFontW(
            -56, 0, 0, 0, FW_BOLD, FALSE, FALSE, FALSE,
            DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
            CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI");
        HFONT oldFont = (HFONT)SelectObject(memDC, numFont);

        std::wstring numStr = std::to_wstring(count);
        RECT numRc = { 0, 16, rc.right, rc.bottom - 36 };
        DrawTextW(memDC, numStr.c_str(), -1, &numRc, DT_CENTER | DT_VCENTER | DT_SINGLELINE);

        SelectObject(memDC, oldFont);
        DeleteObject(numFont);

        HFONT hintFont = CreateFontW(
            -13, 0, 0, 0, FW_SEMIBOLD, FALSE, FALSE, FALSE,
            DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
            CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI");
        oldFont = (HFONT)SelectObject(memDC, hintFont);
        SetTextColor(memDC, RGB(180, 185, 200));

        std::wstring hintStr = L"Space 跳过";
        RECT hintRc = { 0, rc.bottom - 34, rc.right, rc.bottom - 8 };
        DrawTextW(memDC, hintStr.c_str(), -1, &hintRc, DT_CENTER | DT_VCENTER | DT_SINGLELINE);

        SelectObject(memDC, oldFont);
        DeleteObject(hintFont);

        HPEN ringPen = CreatePen(PS_SOLID, 3, RGB(58, 134, 255));
        HGDIOBJ oldPen = SelectObject(memDC, ringPen);
        HGDIOBJ oldB = SelectObject(memDC, GetStockObject(NULL_BRUSH));
        Ellipse(memDC, 3, 3, rc.right - 3, rc.bottom - 3);
        SelectObject(memDC, oldB);
        SelectObject(memDC, oldPen);
        DeleteObject(ringPen);

        BitBlt(hdc, 0, 0, rc.right, rc.bottom, memDC, 0, 0, SRCCOPY);
        SelectObject(memDC, oldBm);
        DeleteObject(memBm);
        DeleteDC(memDC);

        EndPaint(hwnd, &ps);
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

bool RecordingIndicator::createCountdownWindow(HINSTANCE hInstance) {
    if (m_countdownHwnd) return true;

    if (!s_countdownClassRegistered) {
        WNDCLASSEXW wc{};
        wc.cbSize = sizeof(wc);
        wc.style = CS_HREDRAW | CS_VREDRAW;
        wc.lpfnWndProc = countdownWndProc;
        wc.hInstance = hInstance;
        wc.lpszClassName = COUNTDOWN_CLASS;
        RegisterClassExW(&wc);
        s_countdownClassRegistered = true;
    }

    int sz = scaledIndicatorMetric(130, m_dpiScale);
    int cx = 0, cy = 0;
    if (m_hasRecordingRegion) {
        cx = (m_recordingRegion.left + m_recordingRegion.right) / 2;
        cy = (m_recordingRegion.top + m_recordingRegion.bottom) / 2;
    } else {
        cx = GetSystemMetrics(SM_CXSCREEN) / 2;
        cy = GetSystemMetrics(SM_CYSCREEN) / 2;
    }

    int x = cx - sz / 2;
    int y = cy - sz / 2;

    m_countdownHwnd = CreateWindowExW(
        WS_EX_LAYERED | WS_EX_TOOLWINDOW | WS_EX_TOPMOST | WS_EX_NOACTIVATE,
        COUNTDOWN_CLASS, L"",
        WS_POPUP,
        x, y, sz, sz,
        nullptr, nullptr, hInstance, nullptr
    );

    if (!m_countdownHwnd) return false;

    HRGN circleRgn = CreateEllipticRgn(0, 0, sz, sz);
    SetWindowRgn(m_countdownHwnd, circleRgn, TRUE);

    SetLayeredWindowAttributes(m_countdownHwnd, 0, 235, LWA_ALPHA);
    tools3000::core::WinUtils::excludeWindowFromCapture(m_countdownHwnd);

    ShowWindow(m_countdownHwnd, SW_SHOWNOACTIVATE);
    UpdateWindow(m_countdownHwnd);
    return true;
}

void RecordingIndicator::destroyCountdownWindow() {
    if (m_countdownHwnd) {
        ShowWindow(m_countdownHwnd, SW_HIDE);
        DestroyWindow(m_countdownHwnd);
        m_countdownHwnd = nullptr;
    }
}

void RecordingIndicator::updateCountdownWindow() {
    if (m_countingDown) {
        if (!m_countdownHwnd && m_hInstance) {
            createCountdownWindow(m_hInstance);
        }
        if (m_countdownHwnd) {
            InvalidateRect(m_countdownHwnd, nullptr, FALSE);
        }
    } else {
        if (m_countdownHwnd) {
            destroyCountdownWindow();
        }
    }
}

}  // namespace tools3000::capture
