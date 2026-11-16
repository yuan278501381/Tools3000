#include "capture/StudioPreviewWindow.h"
#include "core/config/ConfigManager.h"
#include "core/events/EventBus.h"
#include "core/events/MainThreadDispatcher.h"
#include "core/logger/Logger.h"
#include "core/utils/DWriteFontUtils.h"
#include "core/utils/DpiUtils.h"
#include "core/utils/ThemeUtils.h"
#include "core/utils/TraceId.h"
#include "core/utils/WinUtils.h"
#include "ui/KeyboardPipeline.h"

#include <opencv2/imgproc.hpp>
#include <windowsx.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <filesystem>

namespace tools3000::capture {

namespace {

using Microsoft::WRL::ComPtr;

constexpr const wchar_t* kStudioWndClass = L"Tools3000_StudioPreviewWindow";
constexpr UINT_PTR kPlaybackTimerId = 4101;
constexpr UINT kTimerIntervalMs = 33; // ~30 FPS 丝滑实时预览

enum HitId : int {
    Hit_None = 0,
    Hit_CloseBtn = 1,
    Hit_PlayPauseBtn = 2,
    Hit_TimelineScrub = 3,
    Hit_ToggleAutoZoom = 10,
    Hit_ZoomPreset_150 = 11,
    Hit_ZoomPreset_185 = 12,
    Hit_ZoomPreset_220 = 13,
    Hit_ToggleSmoothCursor = 20,
    Hit_CursorScale_100 = 21,
    Hit_CursorScale_135 = 22,
    Hit_CursorScale_170 = 23,
    Hit_ToggleAutoFrame = 30,
    Hit_ThemeBase = 40, // 40..44 对应 5 套壁纸主题
    Hit_ToggleCaptions = 50,
    Hit_ExportStudioBtn = 60,
    Hit_KeepOriginalBtn = 61,
    Hit_ZoomSegmentBase = 1000, // 1000 + idx 切换单个关键帧放大块启停
};

std::wstring formatTimecodeMmSs(double sec) {
    if (sec < 0.0 || !std::isfinite(sec)) sec = 0.0;
    const int totalTenths = static_cast<int>(std::round(sec * 10.0));
    const int mins = (totalTenths / 10) / 60;
    const int secs = (totalTenths / 10) % 60;
    const int tenths = totalTenths % 10;
    wchar_t buf[32]{};
    std::swprintf(buf, std::size(buf), L"%02d:%02d.%d", mins, secs, tenths);
    return buf;
}

bool pointInRectF(const D2D1_RECT_F& r, float x, float y) noexcept {
    return x >= r.left && x <= r.right && y >= r.top && y <= r.bottom;
}

std::string themeKeyFromEnum(BeautyBackgroundType t) {
    switch (t) {
        case BeautyBackgroundType::PaperChalk:       return "sunrise_coral";
        case BeautyBackgroundType::SilkMist:         return "emerald_sea";
        case BeautyBackgroundType::MidnightGraphite: return "obsidian_dark";
        case BeautyBackgroundType::PureMinimal:      return "minimal_light";
        case BeautyBackgroundType::StudioSlate:
        default:                                     return "aurora_purple";
    }
}

D2D1_COLOR_F swatchColorForTheme(BeautyBackgroundType t) noexcept {
    switch (t) {
        case BeautyBackgroundType::PaperChalk:       return D2D1::ColorF(0.976f, 0.451f, 0.086f, 1.0f); // #F97316
        case BeautyBackgroundType::SilkMist:         return D2D1::ColorF(0.078f, 0.722f, 0.651f, 1.0f); // #14B8A6
        case BeautyBackgroundType::MidnightGraphite: return D2D1::ColorF(0.118f, 0.161f, 0.231f, 1.0f); // #1E293B
        case BeautyBackgroundType::PureMinimal:      return D2D1::ColorF(0.886f, 0.910f, 0.941f, 1.0f); // #E2E8F0
        case BeautyBackgroundType::StudioSlate:
        default:                                     return D2D1::ColorF(0.545f, 0.361f, 0.965f, 1.0f); // #8B5CF6
    }
}

void persistStudioConfigToSettings(const StudioRenderConfig& cfg) {
    auto& cm = tools3000::core::ConfigManager::instance();
    cm.set<bool>("/recording/studioAutoZoom", cfg.autoZoomEnabled);
    cm.set<double>("/recording/studioZoomFactor", static_cast<double>(cfg.defaultZoomFactor));
    cm.set<bool>("/recording/studioSmoothCursor", cfg.smoothCursorEnabled);
    cm.set<double>("/recording/studioCursorScale", static_cast<double>(cfg.cursorScale));
    cm.set<bool>("/recording/studioAutoFrame", cfg.autoFrameEnabled);
    cm.set<std::string>("/recording/studioTheme", themeKeyFromEnum(cfg.backgroundTheme));
    cm.set<bool>("/recording/studioAutoCaptions", cfg.autoCaptionsEnabled);
}

} // namespace

StudioPreviewWindow& StudioPreviewWindow::instance() {
    static StudioPreviewWindow inst;
    return inst;
}

bool StudioPreviewWindow::isVisible() const noexcept {
    return m_hwnd && IsWindow(m_hwnd) && IsWindowVisible(m_hwnd);
}

bool StudioPreviewWindow::ensureWindowCreated() {
    if (m_hwnd && IsWindow(m_hwnd)) return true;

    HINSTANCE hInst = GetModuleHandleW(nullptr);
    static bool s_registered = false;
    if (!s_registered) {
        WNDCLASSEXW wc{};
        wc.cbSize = sizeof(wc);
        wc.style = CS_HREDRAW | CS_VREDRAW | CS_DBLCLKS;
        wc.lpfnWndProc = &StudioPreviewWindow::wndProc;
        wc.hInstance = hInst;
        wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        wc.lpszClassName = kStudioWndClass;
        if (RegisterClassExW(&wc) || GetLastError() == ERROR_CLASS_ALREADY_EXISTS) {
            s_registered = true;
        }
    }

    const HMONITOR mon = tools3000::core::dpi::activeMonitor();
    m_dpiScale = tools3000::core::dpi::scaleForMonitor(mon);
    const RECT work = tools3000::core::dpi::workArea(mon);
    const SIZE desiredPhys{
        tools3000::core::dpi::scaleMetric(m_logicalWidth, m_dpiScale),
        tools3000::core::dpi::scaleMetric(m_logicalHeight, m_dpiScale)
    };
    const SIZE fitted = tools3000::core::dpi::fitSizeToWorkArea(
        desiredPhys, work, tools3000::core::dpi::scaleMetric(24, m_dpiScale));
    const int posX = work.left + ((work.right - work.left) - fitted.cx) / 2;
    const int posY = work.top + ((work.bottom - work.top) - fitted.cy) / 2;

    m_hwnd = CreateWindowExW(
        WS_EX_APPWINDOW,
        kStudioWndClass,
        L"Tools3000 Studio",
        WS_POPUP | WS_CLIPCHILDREN | WS_SYSMENU | WS_MINIMIZEBOX,
        posX, posY, fitted.cx, fitted.cy,
        nullptr, nullptr, hInst, this);
    if (!m_hwnd) {
        LOG_ERROR("StudioPreviewWindow: CreateWindowExW failed, err={}", GetLastError());
        return false;
    }

    tools3000::core::WinUtils::allowWindowCapture(m_hwnd);
    tools3000::core::WinUtils::applyUniversalRoundedCorners(
        m_hwnd, fitted.cx, fitted.cy, tools3000::core::dpi::scaleMetric(14, m_dpiScale));
    return true;
}

bool StudioPreviewWindow::showSession(
    const std::string& rawVideoPathUtf8,
    StudioTimelineData timeline,
    const StudioRenderConfig& initialConfig,
    int bitrateMbps,
    CompletionHandler onFinished) {
    if (rawVideoPathUtf8.empty()) return false;

    joinExportWorkerIfDone(true);

    std::vector<cv::Mat> deck;
    int origW = 0;
    int origH = 0;
    double durSec = 0.0;
    if (!StudioVideoCompositor::extractPreviewDeck(rawVideoPathUtf8, 760, 180, deck, origW, origH, durSec) ||
        deck.empty()) {
        LOG_WARN("StudioPreviewWindow: failed to extract preview deck from {}, falling back to direct raw completion",
                 rawVideoPathUtf8);
        if (onFinished) {
            onFinished(rawVideoPathUtf8, timeline.durationMs / 1000.0, false);
        }
        return false;
    }

    if (!ensureWindowCreated()) {
        if (onFinished) {
            onFinished(rawVideoPathUtf8, durSec, false);
        }
        return false;
    }

    m_rawVideoPath = rawVideoPathUtf8;
    m_timeline = std::move(timeline);
    if (m_timeline.sourceWidth <= 0) m_timeline.sourceWidth = origW;
    if (m_timeline.sourceHeight <= 0) m_timeline.sourceHeight = origH;
    if (m_timeline.durationMs <= 0 && durSec > 0.0) {
        m_timeline.durationMs = static_cast<int64_t>(std::round(durSec * 1000.0));
    }
    m_config = initialConfig;
    if (m_timeline.zoomSegments.empty()) {
        m_timeline.zoomSegments = StudioTimelineAnalyzer::buildAutoZoomSegments(
            m_timeline, m_config.defaultZoomFactor);
    }

    m_bitrateMbps = std::clamp(bitrateMbps, 2, 80);
    m_durationSec = (durSec > 0.05) ? durSec : (std::max)(0.1, m_timeline.durationMs / 1000.0);
    m_onFinished = std::move(onFinished);
    m_previewDeckBgr = std::move(deck);
    m_origWidth = origW;
    m_origHeight = origH;

    m_exporting.store(false, std::memory_order_release);
    m_cancelExport.store(false, std::memory_order_release);
    m_exportProgress.store(0.0, std::memory_order_release);

    m_playheadSec = 0.0;
    m_playing = true;
    m_scrubbingTimeline = false;
    m_lastTickMs = GetTickCount();

    rebakeTimelinePreview();
    updateCurrentComposedBitmap();

    const HMONITOR mon = tools3000::core::dpi::activeMonitor();
    m_dpiScale = tools3000::core::dpi::scaleForMonitor(mon);
    const RECT work = tools3000::core::dpi::workArea(mon);
    const SIZE desiredPhys{
        tools3000::core::dpi::scaleMetric(m_logicalWidth, m_dpiScale),
        tools3000::core::dpi::scaleMetric(m_logicalHeight, m_dpiScale)
    };
    const SIZE fitted = tools3000::core::dpi::fitSizeToWorkArea(
        desiredPhys, work, tools3000::core::dpi::scaleMetric(24, m_dpiScale));
    const int posX = work.left + ((work.right - work.left) - fitted.cx) / 2;
    const int posY = work.top + ((work.bottom - work.top) - fitted.cy) / 2;

    SetWindowPos(m_hwnd, HWND_TOP, posX, posY, fitted.cx, fitted.cy, SWP_SHOWWINDOW);
    tools3000::core::WinUtils::applyUniversalRoundedCorners(
        m_hwnd, fitted.cx, fitted.cy, tools3000::core::dpi::scaleMetric(14, m_dpiScale));
    SetForegroundWindow(m_hwnd);
    SetTimer(m_hwnd, kPlaybackTimerId, kTimerIntervalMs, nullptr);
    InvalidateRect(m_hwnd, nullptr, FALSE);

    LOG_INFO("StudioPreviewWindow opened: raw={}, deckFrames={}, zoomSegments={}, captions={}",
             m_rawVideoPath, m_previewDeckBgr.size(),
             m_timeline.zoomSegments.size(), m_timeline.captions.size());
    return true;
}

void StudioPreviewWindow::rebakeTimelinePreview() {
    m_previewEvalFps = 30;
    const int totalEvalFrames = (std::max)(
        2, static_cast<int>(std::ceil(m_durationSec * m_previewEvalFps)) + 2);
    m_evaluatedFrames = StudioTimelineAnalyzer::bakeEvaluatedFrames(
        m_timeline, m_config, m_previewEvalFps, totalEvalFrames);
    m_composedDirty = true;
}

void StudioPreviewWindow::updateCurrentComposedBitmap() {
    if (m_previewDeckBgr.empty()) return;

    const double norm = (m_durationSec > 1e-4)
        ? std::clamp(m_playheadSec / m_durationSec, 0.0, 1.0)
        : 0.0;
    const size_t deckIdx = (std::min)(
        m_previewDeckBgr.size() - 1,
        static_cast<size_t>(std::llround(norm * static_cast<double>(m_previewDeckBgr.size() - 1))));
    const size_t evalIdx = m_evaluatedFrames.empty()
        ? 0
        : (std::min)(
            m_evaluatedFrames.size() - 1,
            static_cast<size_t>(std::llround(m_playheadSec * m_previewEvalFps)));

    const StudioEvaluatedFrame fallbackEval{};
    const StudioEvaluatedFrame& eval = m_evaluatedFrames.empty() ? fallbackEval : m_evaluatedFrames[evalIdx];

    const cv::Mat& srcFrame = m_previewDeckBgr[deckIdx];
    cv::Mat composedBgr = m_previewCompositor.composeFrame(
        srcFrame, eval, m_config, cv::Size(srcFrame.cols, srcFrame.rows));
    if (!composedBgr.empty()) {
        cv::cvtColor(composedBgr, m_currentComposedBgra, cv::COLOR_BGR2BGRA);
        m_composedDirty = true;
    }
}

bool StudioPreviewWindow::ensureRenderResources() {
    if (!m_hwnd || !IsWindow(m_hwnd)) return false;

    if (!m_d2dFactory) {
        if (FAILED(D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, m_d2dFactory.GetAddressOf()))) {
            return false;
        }
    }
    if (!m_dwriteFactory) {
        if (FAILED(DWriteCreateFactory(
                DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory),
                reinterpret_cast<IUnknown**>(m_dwriteFactory.GetAddressOf())))) {
            return false;
        }
    }
    if (!m_titleFmt) {
        const wchar_t* uiFont = tools3000::core::resolveInstalledUiFontFamily(m_dwriteFactory.Get());
        const wchar_t* monoFont = tools3000::core::resolveInstalledMonoFontFamily(m_dwriteFactory.Get());
        m_dwriteFactory->CreateTextFormat(
            uiFont, nullptr, DWRITE_FONT_WEIGHT_BOLD,
            DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, 16.0f, L"zh-CN", m_titleFmt.GetAddressOf());
        m_dwriteFactory->CreateTextFormat(
            uiFont, nullptr, DWRITE_FONT_WEIGHT_SEMI_BOLD,
            DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, 13.5f, L"zh-CN", m_sectionFmt.GetAddressOf());
        m_dwriteFactory->CreateTextFormat(
            uiFont, nullptr, DWRITE_FONT_WEIGHT_MEDIUM,
            DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, 12.5f, L"zh-CN", m_bodyFmt.GetAddressOf());
        m_dwriteFactory->CreateTextFormat(
            uiFont, nullptr, DWRITE_FONT_WEIGHT_MEDIUM,
            DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, 12.0f, L"zh-CN", m_captionFmt.GetAddressOf());
        m_dwriteFactory->CreateTextFormat(
            monoFont, nullptr, DWRITE_FONT_WEIGHT_SEMI_BOLD,
            DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, 12.0f, L"en-US", m_monoFmt.GetAddressOf());
    }

    if (!m_renderTarget) {
        RECT rc{};
        GetClientRect(m_hwnd, &rc);
        const UINT physW = static_cast<UINT>((std::max)(1L, rc.right - rc.left));
        const UINT physH = static_cast<UINT>((std::max)(1L, rc.bottom - rc.top));
        D2D1_RENDER_TARGET_PROPERTIES rtProps = D2D1::RenderTargetProperties();
        rtProps.dpiX = 96.0f * m_dpiScale;
        rtProps.dpiY = 96.0f * m_dpiScale;
        if (FAILED(m_d2dFactory->CreateHwndRenderTarget(
                rtProps,
                D2D1::HwndRenderTargetProperties(m_hwnd, D2D1::SizeU(physW, physH)),
                m_renderTarget.GetAddressOf()))) {
            return false;
        }
        m_renderTarget->SetTextAntialiasMode(D2D1_TEXT_ANTIALIAS_MODE_CLEARTYPE);
    }

    if (!m_currentComposedBgra.empty() && m_composedDirty) {
        const UINT32 w = static_cast<UINT32>(m_currentComposedBgra.cols);
        const UINT32 h = static_cast<UINT32>(m_currentComposedBgra.rows);
        if (!m_frameBitmap || m_bitmapW != w || m_bitmapH != h) {
            m_frameBitmap.Reset();
            D2D1_BITMAP_PROPERTIES bp = D2D1::BitmapProperties(
                D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_IGNORE),
                96.0f, 96.0f);
            if (SUCCEEDED(m_renderTarget->CreateBitmap(
                    D2D1::SizeU(w, h),
                    m_currentComposedBgra.data,
                    static_cast<UINT32>(m_currentComposedBgra.step[0]),
                    &bp,
                    m_frameBitmap.GetAddressOf()))) {
                m_bitmapW = w;
                m_bitmapH = h;
                m_composedDirty = false;
            }
        } else {
            D2D1_RECT_U dstU = D2D1::RectU(0, 0, w, h);
            m_frameBitmap->CopyFromMemory(
                &dstU, m_currentComposedBgra.data, static_cast<UINT32>(m_currentComposedBgra.step[0]));
            m_composedDirty = false;
        }
    }

    return m_renderTarget != nullptr;
}

void StudioPreviewWindow::discardDeviceResources() {
    m_frameBitmap.Reset();
    m_bitmapW = 0;
    m_bitmapH = 0;
    m_renderTarget.Reset();
    m_composedDirty = true;
}

void StudioPreviewWindow::render() {
    if (!ensureRenderResources()) return;

    const bool dark = tools3000::core::NativeThemePalette::isEffectiveDarkMode();
    const bool isZh = tools3000::core::NativeThemePalette::isUiLanguageChinese();
    const auto accentRgb = tools3000::core::NativeThemePalette::getEffectiveAccentColorRGB();

    const D2D1_SIZE_F sz = m_renderTarget->GetSize();
    const float W = sz.width;
    const float H = sz.height;

    m_hitZones.clear();

    m_renderTarget->BeginDraw();

    const D2D1_COLOR_F colWindowBg = dark
        ? D2D1::ColorF(0.059f, 0.075f, 0.110f, 1.0f)   // #0F131C
        : D2D1::ColorF(0.969f, 0.976f, 0.988f, 1.0f);  // #F7F9FC
    const D2D1_COLOR_F colCardBg = dark
        ? D2D1::ColorF(0.094f, 0.118f, 0.169f, 1.0f)   // #181E2B
        : D2D1::ColorF(1.000f, 1.000f, 1.000f, 1.0f);
    const D2D1_COLOR_F colStageBg = dark
        ? D2D1::ColorF(0.039f, 0.047f, 0.071f, 1.0f)
        : D2D1::ColorF(0.925f, 0.941f, 0.965f, 1.0f);
    const D2D1_COLOR_F colBorder = dark
        ? D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.10f)
        : D2D1::ColorF(0.059f, 0.090f, 0.165f, 0.11f);
    const D2D1_COLOR_F colPrimary = dark
        ? D2D1::ColorF(0.96f, 0.97f, 0.99f, 1.0f)
        : D2D1::ColorF(0.059f, 0.090f, 0.165f, 1.0f);
    const D2D1_COLOR_F colSecondary = dark
        ? D2D1::ColorF(0.62f, 0.68f, 0.78f, 1.0f)
        : D2D1::ColorF(0.32f, 0.38f, 0.47f, 1.0f);
    const D2D1_COLOR_F colAccent = D2D1::ColorF(accentRgb.r, accentRgb.g, accentRgb.b, 1.0f);
    const D2D1_COLOR_F colAccentSoft = D2D1::ColorF(accentRgb.r, accentRgb.g, accentRgb.b, dark ? 0.22f : 0.13f);
    const D2D1_COLOR_F colChipBg = dark
        ? D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.06f)
        : D2D1::ColorF(0.059f, 0.090f, 0.165f, 0.06f);

    m_renderTarget->Clear(colWindowBg);

    ComPtr<ID2D1SolidColorBrush> brush;
    m_renderTarget->CreateSolidColorBrush(colPrimary, brush.GetAddressOf());
    if (!brush) {
        m_renderTarget->EndDraw();
        return;
    }

    auto setBrush = [&](const D2D1_COLOR_F& c) { brush->SetColor(c); };

    auto drawTextAligned = [&](const std::wstring& txt, IDWriteTextFormat* fmt, const D2D1_RECT_F& r,
                               const D2D1_COLOR_F& col,
                               DWRITE_TEXT_ALIGNMENT hAlign = DWRITE_TEXT_ALIGNMENT_LEADING,
                               DWRITE_PARAGRAPH_ALIGNMENT vAlign = DWRITE_PARAGRAPH_ALIGNMENT_CENTER) {
        if (!fmt || txt.empty()) return;
        fmt->SetTextAlignment(hAlign);
        fmt->SetParagraphAlignment(vAlign);
        setBrush(col);
        m_renderTarget->DrawText(txt.c_str(), static_cast<UINT32>(txt.size()), fmt, r, brush.Get());
    };

    // 1. 顶部标题栏 (0..48px)
    const float headerH = 48.0f;
    const std::wstring headerTitle = isZh
        ? L"演播室录屏成片中心 · Studio Auto-Edit"
        : L"Studio Recording Post-Edit";
    drawTextAligned(headerTitle, m_titleFmt.Get(), D2D1::RectF(20.0f, 0.0f, W - 120.0f, headerH), colPrimary);

    wchar_t metaBuf[128]{};
    std::swprintf(
        metaBuf, std::size(metaBuf),
        isZh ? L"%dx%d · %.1f 秒 · 空格键播放/暂停" : L"%dx%d · %.1fs · Space to Play/Pause",
        m_origWidth, m_origHeight, m_durationSec);
    drawTextAligned(metaBuf, m_captionFmt.Get(), D2D1::RectF(340.0f, 0.0f, W - 64.0f, headerH),
                    colSecondary, DWRITE_TEXT_ALIGNMENT_TRAILING);

    // 右上角关闭按钮 [X]
    const D2D1_RECT_F closeRc = D2D1::RectF(W - 44.0f, 10.0f, W - 16.0f, 38.0f);
    m_hitZones.push_back({Hit_CloseBtn, closeRc, -1});
    if (m_hoveredHitId == Hit_CloseBtn) {
        setBrush(D2D1::ColorF(0.937f, 0.267f, 0.267f, 0.18f));
        m_renderTarget->FillRoundedRectangle(D2D1::RoundedRect(closeRc, 7.0f, 7.0f), brush.Get());
    }
    setBrush(m_hoveredHitId == Hit_CloseBtn ? D2D1::ColorF(0.937f, 0.267f, 0.267f, 1.0f) : colSecondary);
    const float cx = (closeRc.left + closeRc.right) * 0.5f;
    const float cy = (closeRc.top + closeRc.bottom) * 0.5f;
    m_renderTarget->DrawLine(D2D1::Point2F(cx - 5.0f, cy - 5.0f), D2D1::Point2F(cx + 5.0f, cy + 5.0f), brush.Get(), 1.8f);
    m_renderTarget->DrawLine(D2D1::Point2F(cx + 5.0f, cy - 5.0f), D2D1::Point2F(cx - 5.0f, cy + 5.0f), brush.Get(), 1.8f);

    setBrush(colBorder);
    m_renderTarget->DrawLine(D2D1::Point2F(0.0f, headerH), D2D1::Point2F(W, headerH), brush.Get(), 1.0f);

    // 主体左右布局：左侧预览舞台 + 时间轴，右侧参数控制舱
    const float rightPanelW = 306.0f;
    const float leftAreaLeft = 18.0f;
    const float leftAreaRight = W - rightPanelW - 14.0f;
    const float timelineCardH = 112.0f;
    const float stageTop = headerH + 14.0f;
    const float stageBottom = H - timelineCardH - 26.0f;
    const D2D1_RECT_F stageRc = D2D1::RectF(leftAreaLeft, stageTop, leftAreaRight, stageBottom);

    // 2. 左侧视频预览舞台
    setBrush(colStageBg);
    m_renderTarget->FillRoundedRectangle(D2D1::RoundedRect(stageRc, 12.0f, 12.0f), brush.Get());
    setBrush(colBorder);
    m_renderTarget->DrawRoundedRectangle(D2D1::RoundedRect(stageRc, 12.0f, 12.0f), brush.Get(), 1.0f);

    if (m_frameBitmap && m_bitmapW > 0 && m_bitmapH > 0) {
        const float pad = 12.0f;
        const float availW = (std::max)(32.0f, (stageRc.right - stageRc.left) - pad * 2.0f);
        const float availH = (std::max)(32.0f, (stageRc.bottom - stageRc.top) - pad * 2.0f);
        const float fit = (std::min)(availW / static_cast<float>(m_bitmapW), availH / static_cast<float>(m_bitmapH));
        const float drawW = static_cast<float>(m_bitmapW) * fit;
        const float drawH = static_cast<float>(m_bitmapH) * fit;
        const float dx = stageRc.left + ((stageRc.right - stageRc.left) - drawW) * 0.5f;
        const float dy = stageRc.top + ((stageRc.bottom - stageRc.top) - drawH) * 0.5f;
        m_renderTarget->DrawBitmap(
            m_frameBitmap.Get(),
            D2D1::RectF(dx, dy, dx + drawW, dy + drawH),
            1.0f,
            D2D1_BITMAP_INTERPOLATION_MODE_LINEAR);
    }

    // 3. 左下角双轨可视化时间轴卡片（进度条 + 可点击蓝色关键帧放大块 + 琥珀色字幕块）
    const D2D1_RECT_F tlCard = D2D1::RectF(leftAreaLeft, stageBottom + 10.0f, leftAreaRight, H - 16.0f);
    setBrush(colCardBg);
    m_renderTarget->FillRoundedRectangle(D2D1::RoundedRect(tlCard, 12.0f, 12.0f), brush.Get());
    setBrush(colBorder);
    m_renderTarget->DrawRoundedRectangle(D2D1::RoundedRect(tlCard, 12.0f, 12.0f), brush.Get(), 1.0f);

    // 播放/暂停矢量按钮
    const D2D1_RECT_F playBtn = D2D1::RectF(tlCard.left + 14.0f, tlCard.top + 12.0f, tlCard.left + 44.0f, tlCard.top + 42.0f);
    m_hitZones.push_back({Hit_PlayPauseBtn, playBtn, -1});
    setBrush(m_hoveredHitId == Hit_PlayPauseBtn ? colAccent : colAccentSoft);
    m_renderTarget->FillRoundedRectangle(D2D1::RoundedRect(playBtn, 8.0f, 8.0f), brush.Get());
    const D2D1_COLOR_F playIconCol = (m_hoveredHitId == Hit_PlayPauseBtn)
        ? D2D1::ColorF(1.0f, 1.0f, 1.0f, 1.0f)
        : colAccent;
    setBrush(playIconCol);
    const float pcx = (playBtn.left + playBtn.right) * 0.5f;
    const float pcy = (playBtn.top + playBtn.bottom) * 0.5f;
    if (m_playing) {
        m_renderTarget->FillRoundedRectangle(
            D2D1::RoundedRect(D2D1::RectF(pcx - 5.0f, pcy - 6.0f, pcx - 1.8f, pcy + 6.0f), 1.0f, 1.0f), brush.Get());
        m_renderTarget->FillRoundedRectangle(
            D2D1::RoundedRect(D2D1::RectF(pcx + 1.8f, pcy - 6.0f, pcx + 5.0f, pcy + 6.0f), 1.0f, 1.0f), brush.Get());
    } else {
        ComPtr<ID2D1PathGeometry> tri;
        if (SUCCEEDED(m_d2dFactory->CreatePathGeometry(tri.GetAddressOf())) && tri) {
            ComPtr<ID2D1GeometrySink> sink;
            if (SUCCEEDED(tri->Open(sink.GetAddressOf())) && sink) {
                sink->BeginFigure(D2D1::Point2F(pcx - 4.0f, pcy - 6.5f), D2D1_FIGURE_BEGIN_FILLED);
                sink->AddLine(D2D1::Point2F(pcx + 6.0f, pcy));
                sink->AddLine(D2D1::Point2F(pcx - 4.0f, pcy + 6.5f));
                sink->EndFigure(D2D1_FIGURE_END_CLOSED);
                sink->Close();
                m_renderTarget->FillGeometry(tri.Get(), brush.Get());
            }
        }
    }

    // 时间码
    const std::wstring timeStr = formatTimecodeMmSs(m_playheadSec) + L" / " + formatTimecodeMmSs(m_durationSec);
    drawTextAligned(timeStr, m_monoFmt.Get(),
                    D2D1::RectF(playBtn.right + 10.0f, tlCard.top + 12.0f, playBtn.right + 142.0f, tlCard.top + 42.0f),
                    colPrimary);

    // 进度条轨道
    m_timelineTrackRect = D2D1::RectF(playBtn.right + 148.0f, tlCard.top + 23.0f, tlCard.right - 18.0f, tlCard.top + 31.0f);
    const D2D1_RECT_F scrubHitRect = D2D1::RectF(
        m_timelineTrackRect.left, tlCard.top + 12.0f, m_timelineTrackRect.right, tlCard.top + 42.0f);
    m_hitZones.push_back({Hit_TimelineScrub, scrubHitRect, -1});

    setBrush(colChipBg);
    m_renderTarget->FillRoundedRectangle(D2D1::RoundedRect(m_timelineTrackRect, 4.0f, 4.0f), brush.Get());

    const float progressRatio = (m_durationSec > 1e-4)
        ? static_cast<float>(std::clamp(m_playheadSec / m_durationSec, 0.0, 1.0))
        : 0.0f;
    const float playheadX = m_timelineTrackRect.left +
        (m_timelineTrackRect.right - m_timelineTrackRect.left) * progressRatio;

    setBrush(colAccent);
    m_renderTarget->FillRoundedRectangle(
        D2D1::RoundedRect(D2D1::RectF(
            m_timelineTrackRect.left, m_timelineTrackRect.top,
            (std::max)(m_timelineTrackRect.left + 4.0f, playheadX), m_timelineTrackRect.bottom), 4.0f, 4.0f),
        brush.Get());
    m_renderTarget->FillEllipse(D2D1::Ellipse(D2D1::Point2F(playheadX, (m_timelineTrackRect.top + m_timelineTrackRect.bottom) * 0.5f), 6.5f, 6.5f), brush.Get());

    // 关键帧放大轨道 (Auto-Zoom Track: 点击块可单独禁用/启用)
    const float trackW = (std::max)(1.0f, m_timelineTrackRect.right - m_timelineTrackRect.left);
    const float zoomLaneTop = tlCard.top + 52.0f;
    const float zoomLaneBot = tlCard.top + 74.0f;
    drawTextAligned(isZh ? L"运镜关键帧" : L"Auto-Zoom", m_captionFmt.Get(),
                    D2D1::RectF(tlCard.left + 14.0f, zoomLaneTop, m_timelineTrackRect.left - 8.0f, zoomLaneBot),
                    colSecondary);

    setBrush(colChipBg);
    m_renderTarget->FillRoundedRectangle(
        D2D1::RoundedRect(D2D1::RectF(m_timelineTrackRect.left, zoomLaneTop, m_timelineTrackRect.right, zoomLaneBot), 5.0f, 5.0f),
        brush.Get());

    const double totalMs = (std::max)(100.0, m_durationSec * 1000.0);
    if (m_timeline.zoomSegments.empty()) {
        drawTextAligned(
            isZh ? L"全程平稳画面（未检测到密集点击放大点）" : L"Steady full-frame (no click zoom clusters)",
            m_captionFmt.Get(),
            D2D1::RectF(m_timelineTrackRect.left + 10.0f, zoomLaneTop, m_timelineTrackRect.right - 10.0f, zoomLaneBot),
            colSecondary);
    } else {
        for (size_t i = 0; i < m_timeline.zoomSegments.size(); ++i) {
            const auto& seg = m_timeline.zoomSegments[i];
            const float sx = m_timelineTrackRect.left + static_cast<float>(std::clamp(seg.startMs / totalMs, 0.0, 1.0)) * trackW;
            const float ex = m_timelineTrackRect.left + static_cast<float>(std::clamp(seg.endMs / totalMs, 0.0, 1.0)) * trackW;
            const D2D1_RECT_F segRc = D2D1::RectF(sx, zoomLaneTop + 2.0f, (std::max)(sx + 14.0f, ex), zoomLaneBot - 2.0f);
            const int hitId = Hit_ZoomSegmentBase + static_cast<int>(i);
            m_hitZones.push_back({hitId, segRc, static_cast<int>(i)});

            if (seg.enabled && m_config.autoZoomEnabled) {
                setBrush(m_hoveredZoomIdx == static_cast<int>(i)
                    ? D2D1::ColorF(0.29f, 0.56f, 0.99f, 0.95f)
                    : D2D1::ColorF(0.231f, 0.510f, 0.965f, 0.80f));
                m_renderTarget->FillRoundedRectangle(D2D1::RoundedRect(segRc, 4.0f, 4.0f), brush.Get());
            } else {
                setBrush(D2D1::ColorF(0.45f, 0.52f, 0.64f, 0.25f));
                m_renderTarget->FillRoundedRectangle(D2D1::RoundedRect(segRc, 4.0f, 4.0f), brush.Get());
                setBrush(D2D1::ColorF(0.45f, 0.52f, 0.64f, 0.55f));
                m_renderTarget->DrawRoundedRectangle(D2D1::RoundedRect(segRc, 4.0f, 4.0f), brush.Get(), 1.0f);
            }
        }
    }

    // 底部字幕轨标记 (Captions Track)
    const float capLaneTop = tlCard.top + 80.0f;
    const float capLaneBot = tlCard.top + 98.0f;
    drawTextAligned(isZh ? L"对齐字幕轨" : L"Captions", m_captionFmt.Get(),
                    D2D1::RectF(tlCard.left + 14.0f, capLaneTop, m_timelineTrackRect.left - 8.0f, capLaneBot),
                    colSecondary);
    setBrush(colChipBg);
    m_renderTarget->FillRoundedRectangle(
        D2D1::RoundedRect(D2D1::RectF(m_timelineTrackRect.left, capLaneTop + 2.0f, m_timelineTrackRect.right, capLaneBot - 2.0f), 4.0f, 4.0f),
        brush.Get());
    if (m_timeline.captions.empty()) {
        drawTextAligned(
            isZh ? L"点击上方蓝色运镜块可单独开关某段放大" : L"Click blue blocks above to toggle individual zooms",
            m_captionFmt.Get(),
            D2D1::RectF(m_timelineTrackRect.left + 10.0f, capLaneTop - 1.0f, m_timelineTrackRect.right - 10.0f, capLaneBot + 1.0f),
            colSecondary);
    } else {
        for (const auto& cap : m_timeline.captions) {
            const float sx = m_timelineTrackRect.left + static_cast<float>(std::clamp(cap.startMs / totalMs, 0.0, 1.0)) * trackW;
            const float ex = m_timelineTrackRect.left + static_cast<float>(std::clamp(cap.endMs / totalMs, 0.0, 1.0)) * trackW;
            setBrush(m_config.autoCaptionsEnabled
                ? D2D1::ColorF(0.961f, 0.620f, 0.043f, 0.82f)
                : D2D1::ColorF(0.45f, 0.52f, 0.64f, 0.30f));
            m_renderTarget->FillRoundedRectangle(
                D2D1::RoundedRect(D2D1::RectF(sx, capLaneTop + 3.0f, (std::max)(sx + 8.0f, ex), capLaneBot - 3.0f), 3.0f, 3.0f),
                brush.Get());
        }
    }

    // 4. 右侧参数控制舱
    const D2D1_RECT_F sideCard = D2D1::RectF(leftAreaRight + 14.0f, stageTop, W - 18.0f, H - 16.0f);
    setBrush(colCardBg);
    m_renderTarget->FillRoundedRectangle(D2D1::RoundedRect(sideCard, 12.0f, 12.0f), brush.Get());
    setBrush(colBorder);
    m_renderTarget->DrawRoundedRectangle(D2D1::RoundedRect(sideCard, 12.0f, 12.0f), brush.Get(), 1.0f);

    auto drawTogglePill = [&](int hitId, float rightEdge, float topY, bool on) {
        const D2D1_RECT_F rc = D2D1::RectF(rightEdge - 40.0f, topY, rightEdge, topY + 22.0f);
        m_hitZones.push_back({hitId, rc, -1});
        setBrush(on ? colAccent : (dark ? D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.16f) : D2D1::ColorF(0.75f, 0.80f, 0.86f, 1.0f)));
        m_renderTarget->FillRoundedRectangle(D2D1::RoundedRect(rc, 11.0f, 11.0f), brush.Get());
        const float knobX = on ? (rc.right - 11.0f) : (rc.left + 11.0f);
        setBrush(D2D1::ColorF(1.0f, 1.0f, 1.0f, 1.0f));
        m_renderTarget->FillEllipse(D2D1::Ellipse(D2D1::Point2F(knobX, topY + 11.0f), 8.0f, 8.0f), brush.Get());
    };

    auto drawOptionChip = [&](int hitId, const D2D1_RECT_F& rc, const std::wstring& label, bool active) {
        m_hitZones.push_back({hitId, rc, -1});
        setBrush(active ? colAccentSoft : colChipBg);
        m_renderTarget->FillRoundedRectangle(D2D1::RoundedRect(rc, 6.0f, 6.0f), brush.Get());
        setBrush(active ? colAccent : colBorder);
        m_renderTarget->DrawRoundedRectangle(D2D1::RoundedRect(rc, 6.0f, 6.0f), brush.Get(), active ? 1.4f : 1.0f);
        drawTextAligned(label, m_captionFmt.Get(), rc, active ? colAccent : colSecondary, DWRITE_TEXT_ALIGNMENT_CENTER);
    };

    const float padL = sideCard.left + 16.0f;
    const float padR = sideCard.right - 16.0f;
    float curY = sideCard.top + 16.0f;

    // (A) 自动关键帧放大
    drawTextAligned(isZh ? L"关键帧自动放大" : L"Auto-Zoom Camera", m_sectionFmt.Get(),
                    D2D1::RectF(padL, curY, padR - 48.0f, curY + 22.0f), colPrimary);
    drawTogglePill(Hit_ToggleAutoZoom, padR, curY, m_config.autoZoomEnabled);
    curY += 26.0f;
    wchar_t zoomSub[96]{};
    std::swprintf(zoomSub, std::size(zoomSub),
                  isZh ? L"弹簧平滑跟随点击与输入 (%zu 段)" : L"Spring camera on clicks (%zu segs)",
                  m_timeline.zoomSegments.size());
    drawTextAligned(zoomSub, m_captionFmt.Get(), D2D1::RectF(padL, curY, padR, curY + 18.0f), colSecondary);
    curY += 24.0f;
    {
        const float cw = (padR - padL - 16.0f) / 3.0f;
        drawOptionChip(Hit_ZoomPreset_150, D2D1::RectF(padL, curY, padL + cw, curY + 26.0f),
                       L"1.5x", std::abs(m_config.defaultZoomFactor - 1.50f) < 0.12f);
        drawOptionChip(Hit_ZoomPreset_185, D2D1::RectF(padL + cw + 8.0f, curY, padL + cw * 2.0f + 8.0f, curY + 26.0f),
                       L"1.85x", std::abs(m_config.defaultZoomFactor - 1.85f) < 0.15f);
        drawOptionChip(Hit_ZoomPreset_220, D2D1::RectF(padL + cw * 2.0f + 16.0f, curY, padR, curY + 26.0f),
                       L"2.2x", std::abs(m_config.defaultZoomFactor - 2.20f) < 0.15f);
    }
    curY += 38.0f;
    setBrush(colBorder);
    m_renderTarget->DrawLine(D2D1::Point2F(padL, curY), D2D1::Point2F(padR, curY), brush.Get(), 1.0f);
    curY += 12.0f;

    // (B) 光标平滑移动与超清重绘
    drawTextAligned(isZh ? L"光标平滑与超清重绘" : L"Smooth Vector Cursor", m_sectionFmt.Get(),
                    D2D1::RectF(padL, curY, padR - 48.0f, curY + 22.0f), colPrimary);
    drawTogglePill(Hit_ToggleSmoothCursor, padR, curY, m_config.smoothCursorEnabled);
    curY += 26.0f;
    drawTextAligned(isZh ? L"消除手部微抖动，放大后依然锐利" : L"De-jittered crisp vector pointer",
                    m_captionFmt.Get(), D2D1::RectF(padL, curY, padR, curY + 18.0f), colSecondary);
    curY += 24.0f;
    {
        const float cw = (padR - padL - 16.0f) / 3.0f;
        drawOptionChip(Hit_CursorScale_100, D2D1::RectF(padL, curY, padL + cw, curY + 26.0f),
                       isZh ? L"标准 1.0x" : L"1.0x", std::abs(m_config.cursorScale - 1.00f) < 0.15f);
        drawOptionChip(Hit_CursorScale_135, D2D1::RectF(padL + cw + 8.0f, curY, padL + cw * 2.0f + 8.0f, curY + 26.0f),
                       isZh ? L"清晰 1.35x" : L"1.35x", std::abs(m_config.cursorScale - 1.35f) < 0.15f);
        drawOptionChip(Hit_CursorScale_170, D2D1::RectF(padL + cw * 2.0f + 16.0f, curY, padR, curY + 26.0f),
                       isZh ? L"演示 1.7x" : L"1.7x", std::abs(m_config.cursorScale - 1.70f) < 0.15f);
    }
    curY += 38.0f;
    setBrush(colBorder);
    m_renderTarget->DrawLine(D2D1::Point2F(padL, curY), D2D1::Point2F(padR, curY), brush.Get(), 1.0f);
    curY += 12.0f;

    // (C) 自动背景壁纸与圆角阴影美化
    drawTextAligned(isZh ? L"自动壁纸与圆角画框" : L"Wallpaper & Shadow Frame", m_sectionFmt.Get(),
                    D2D1::RectF(padL, curY, padR - 48.0f, curY + 22.0f), colPrimary);
    drawTogglePill(Hit_ToggleAutoFrame, padR, curY, m_config.autoFrameEnabled);
    curY += 30.0f;
    {
        static constexpr std::array<BeautyBackgroundType, 5> kThemes = {{
            BeautyBackgroundType::StudioSlate,
            BeautyBackgroundType::PaperChalk,
            BeautyBackgroundType::SilkMist,
            BeautyBackgroundType::MidnightGraphite,
            BeautyBackgroundType::PureMinimal,
        }};
        for (size_t i = 0; i < kThemes.size(); ++i) {
            const float scx = padL + 16.0f + static_cast<float>(i) * 44.0f;
            const float scy = curY + 13.0f;
            const D2D1_RECT_F swRc = D2D1::RectF(scx - 14.0f, scy - 14.0f, scx + 14.0f, scy + 14.0f);
            m_hitZones.push_back({Hit_ThemeBase + static_cast<int>(i), swRc, static_cast<int>(i)});
            setBrush(swatchColorForTheme(kThemes[i]));
            m_renderTarget->FillEllipse(D2D1::Ellipse(D2D1::Point2F(scx, scy), 11.5f, 11.5f), brush.Get());
            if (m_config.backgroundTheme == kThemes[i]) {
                setBrush(colAccent);
                m_renderTarget->DrawEllipse(D2D1::Ellipse(D2D1::Point2F(scx, scy), 14.5f, 14.5f), brush.Get(), 2.0f);
            } else {
                setBrush(colBorder);
                m_renderTarget->DrawEllipse(D2D1::Ellipse(D2D1::Point2F(scx, scy), 11.5f, 11.5f), brush.Get(), 1.0f);
            }
        }
    }
    curY += 38.0f;
    setBrush(colBorder);
    m_renderTarget->DrawLine(D2D1::Point2F(padL, curY), D2D1::Point2F(padR, curY), brush.Get(), 1.0f);
    curY += 12.0f;

    // (D) 离线语音字幕自动对齐
    drawTextAligned(isZh ? L"语音字幕自动对齐" : L"Auto-Aligned Captions", m_sectionFmt.Get(),
                    D2D1::RectF(padL, curY, padR - 48.0f, curY + 22.0f), colPrimary);
    drawTogglePill(Hit_ToggleCaptions, padR, curY, m_config.autoCaptionsEnabled);
    curY += 25.0f;
    wchar_t capSub[96]{};
    std::swprintf(capSub, std::size(capSub),
                  isZh ? L"已对齐 %zu 条字幕（同步导出 .srt）" : L"%zu synced captions (+ .srt sidecar)",
                  m_timeline.captions.size());
    drawTextAligned(capSub, m_captionFmt.Get(), D2D1::RectF(padL, curY, padR, curY + 18.0f), colSecondary);

    // 底部操作按钮区：导出美化成片 MP4 / 直接保留原片
    const bool exporting = m_exporting.load(std::memory_order_acquire);
    const D2D1_RECT_F keepBtnRc = D2D1::RectF(padL, sideCard.bottom - 44.0f, padR, sideCard.bottom - 12.0f);
    const D2D1_RECT_F exportBtnRc = D2D1::RectF(padL, keepBtnRc.top - 44.0f, padR, keepBtnRc.top - 8.0f);

    if (!exporting) {
        m_hitZones.push_back({Hit_ExportStudioBtn, exportBtnRc, -1});
        m_hitZones.push_back({Hit_KeepOriginalBtn, keepBtnRc, -1});
    }

    setBrush(exporting
        ? D2D1::ColorF(accentRgb.r, accentRgb.g, accentRgb.b, 0.45f)
        : (m_hoveredHitId == Hit_ExportStudioBtn
            ? D2D1::ColorF((std::min)(1.0f, accentRgb.r + 0.06f), (std::min)(1.0f, accentRgb.g + 0.06f), (std::min)(1.0f, accentRgb.b + 0.06f), 1.0f)
            : colAccent));
    m_renderTarget->FillRoundedRectangle(D2D1::RoundedRect(exportBtnRc, 8.0f, 8.0f), brush.Get());

    if (exporting) {
        const double prog = std::clamp(m_exportProgress.load(std::memory_order_relaxed), 0.0, 1.0);
        const float fillW = (exportBtnRc.right - exportBtnRc.left) * static_cast<float>(prog);
        setBrush(colAccent);
        m_renderTarget->FillRoundedRectangle(
            D2D1::RoundedRect(D2D1::RectF(exportBtnRc.left, exportBtnRc.top, exportBtnRc.left + fillW, exportBtnRc.bottom), 8.0f, 8.0f),
            brush.Get());
        wchar_t progTxt[64]{};
        std::swprintf(progTxt, std::size(progTxt),
                      isZh ? L"正在硬件合成导出 %d%%..." : L"Rendering MP4 %d%%...",
                      static_cast<int>(std::round(prog * 100.0)));
        drawTextAligned(progTxt, m_sectionFmt.Get(), exportBtnRc,
                        D2D1::ColorF(1.0f, 1.0f, 1.0f, 1.0f), DWRITE_TEXT_ALIGNMENT_CENTER);
    } else {
        drawTextAligned(isZh ? L"导出美化成片 MP4" : L"Export Studio MP4",
                        m_sectionFmt.Get(), exportBtnRc,
                        D2D1::ColorF(1.0f, 1.0f, 1.0f, 1.0f), DWRITE_TEXT_ALIGNMENT_CENTER);
    }

    setBrush(m_hoveredHitId == Hit_KeepOriginalBtn ? colAccentSoft : colChipBg);
    m_renderTarget->FillRoundedRectangle(D2D1::RoundedRect(keepBtnRc, 8.0f, 8.0f), brush.Get());
    setBrush(colBorder);
    m_renderTarget->DrawRoundedRectangle(D2D1::RoundedRect(keepBtnRc, 8.0f, 8.0f), brush.Get(), 1.0f);
    drawTextAligned(isZh ? L"直接保留原片 (0 秒等待)" : L"Keep Raw MP4 Instantly",
                    m_bodyFmt.Get(), keepBtnRc,
                    exporting ? colSecondary : colPrimary, DWRITE_TEXT_ALIGNMENT_CENTER);

    if (m_renderTarget->EndDraw() == D2DERR_RECREATE_TARGET) {
        discardDeviceResources();
    }
}

void StudioPreviewWindow::onPlaybackTick() {
    if (!isVisible()) return;

    const DWORD now = GetTickCount();
    const double dt = std::clamp(static_cast<double>(now - m_lastTickMs) / 1000.0, 0.0, 0.15);
    m_lastTickMs = now;

    if (m_exporting.load(std::memory_order_acquire)) {
        InvalidateRect(m_hwnd, nullptr, FALSE);
        return;
    }

    if (m_playing && !m_scrubbingTimeline && m_durationSec > 0.05) {
        m_playheadSec += dt;
        if (m_playheadSec >= m_durationSec) {
            m_playheadSec = 0.0;
        }
        updateCurrentComposedBitmap();
        InvalidateRect(m_hwnd, nullptr, FALSE);
    }
}

void StudioPreviewWindow::startAsyncExport() {
    if (m_exporting.exchange(true, std::memory_order_acq_rel)) return;

    m_playing = false;
    m_cancelExport.store(false, std::memory_order_release);
    m_exportProgress.store(0.01, std::memory_order_release);
    persistStudioConfigToSettings(m_config);
    InvalidateRect(m_hwnd, nullptr, FALSE);

    joinExportWorkerIfDone(true);

    const std::string rawPath = m_rawVideoPath;
    const StudioTimelineData timelineCopy = m_timeline;
    const StudioRenderConfig configCopy = m_config;
    const int bitrate = m_bitrateMbps;
    const double durSec = m_durationSec;

    std::lock_guard lock(m_exportMutex);
    m_exportWorker = std::thread([this, rawPath, timelineCopy, configCopy, bitrate, durSec]() {
        tools3000::core::TraceId::Scope traceScope;
        const bool ok = StudioVideoCompositor::exportRenderedVideo(
            rawPath,
            rawPath,
            timelineCopy,
            configCopy,
            bitrate,
            [this](double p) {
                m_exportProgress.store(p, std::memory_order_relaxed);
            },
            &m_cancelExport);

        const bool canceled = m_cancelExport.load(std::memory_order_acquire);
        m_exporting.store(false, std::memory_order_release);

        tools3000::core::MainThreadDispatcher::instance().post([this, ok, canceled, rawPath, durSec]() {
            if (canceled) return;
            const bool isZh = tools3000::core::NativeThemePalette::isUiLanguageChinese();
            if (ok) {
                LOG_INFO("StudioPreviewWindow: exported studio video successfully: {}", rawPath);
                auto cb = std::move(m_onFinished);
                closeSessionAndCleanup();
                if (cb) cb(rawPath, durSec, true);
            } else {
                LOG_WARN("StudioPreviewWindow: exportRenderedVideo failed, keeping raw video: {}", rawPath);
                tools3000::core::EventBus::instance().publish(tools3000::core::ShowToastEvent{
                    isZh ? L"演播室合成失败，已为您安全保留原始录屏" : L"Studio export failed; raw recording preserved"});
                InvalidateRect(m_hwnd, nullptr, FALSE);
            }
        });
    });
}

void StudioPreviewWindow::keepOriginalAndClose() {
    if (m_exporting.load(std::memory_order_acquire)) return;
    persistStudioConfigToSettings(m_config);
    const std::string path = m_rawVideoPath;
    const double dur = m_durationSec;
    auto cb = std::move(m_onFinished);
    closeSessionAndCleanup();
    if (cb) {
        cb(path, dur, false);
    }
}

void StudioPreviewWindow::joinExportWorkerIfDone(bool wait) {
    std::thread toJoin;
    {
        std::lock_guard lock(m_exportMutex);
        if (m_exportWorker.joinable() && (wait || !m_exporting.load(std::memory_order_acquire))) {
            if (std::this_thread::get_id() != m_exportWorker.get_id()) {
                toJoin = std::move(m_exportWorker);
            }
        }
    }
    if (toJoin.joinable()) {
        toJoin.join();
    }
}

void StudioPreviewWindow::closeSessionAndCleanup() {
    if (m_hwnd && IsWindow(m_hwnd)) {
        KillTimer(m_hwnd, kPlaybackTimerId);
        ShowWindow(m_hwnd, SW_HIDE);
    }
    m_playing = false;
    m_scrubbingTimeline = false;
    m_draggingWindow = false;
    m_previewDeckBgr.clear();
    m_previewDeckBgr.shrink_to_fit();
    m_evaluatedFrames.clear();
    m_evaluatedFrames.shrink_to_fit();
    m_currentComposedBgra.release();
    discardDeviceResources();
    tools3000::core::WinUtils::trimWorkingSet();
}

void StudioPreviewWindow::shutdown() {
    m_cancelExport.store(true, std::memory_order_release);
    joinExportWorkerIfDone(true);
    closeSessionAndCleanup();
    if (m_hwnd && IsWindow(m_hwnd)) {
        DestroyWindow(m_hwnd);
    }
    m_hwnd = nullptr;
}

void StudioPreviewWindow::handleMouseDown(float lx, float ly) {
    for (auto it = m_hitZones.rbegin(); it != m_hitZones.rend(); ++it) {
        if (!pointInRectF(it->rect, lx, ly)) continue;

        const int id = it->id;
        if (id == Hit_CloseBtn || id == Hit_KeepOriginalBtn) {
            if (m_exporting.load(std::memory_order_acquire)) {
                m_cancelExport.store(true, std::memory_order_release);
                joinExportWorkerIfDone(true);
                m_exporting.store(false, std::memory_order_release);
            }
            keepOriginalAndClose();
            return;
        }
        if (id == Hit_ExportStudioBtn) {
            startAsyncExport();
            return;
        }
        if (id == Hit_PlayPauseBtn) {
            m_playing = !m_playing;
            m_lastTickMs = GetTickCount();
            InvalidateRect(m_hwnd, nullptr, FALSE);
            return;
        }
        if (id == Hit_TimelineScrub) {
            m_scrubbingTimeline = true;
            SetCapture(m_hwnd);
            const float w = (std::max)(1.0f, m_timelineTrackRect.right - m_timelineTrackRect.left);
            const double ratio = std::clamp(static_cast<double>((lx - m_timelineTrackRect.left) / w), 0.0, 1.0);
            m_playheadSec = ratio * m_durationSec;
            updateCurrentComposedBitmap();
            InvalidateRect(m_hwnd, nullptr, FALSE);
            return;
        }
        if (id == Hit_ToggleAutoZoom) {
            m_config.autoZoomEnabled = !m_config.autoZoomEnabled;
            rebakeTimelinePreview();
            updateCurrentComposedBitmap();
            InvalidateRect(m_hwnd, nullptr, FALSE);
            return;
        }
        if (id == Hit_ZoomPreset_150 || id == Hit_ZoomPreset_185 || id == Hit_ZoomPreset_220) {
            const float z = (id == Hit_ZoomPreset_150) ? 1.50f : ((id == Hit_ZoomPreset_185) ? 1.85f : 2.20f);
            m_config.defaultZoomFactor = z;
            m_config.autoZoomEnabled = true;
            for (auto& seg : m_timeline.zoomSegments) {
                seg.targetZoom = z;
            }
            rebakeTimelinePreview();
            updateCurrentComposedBitmap();
            InvalidateRect(m_hwnd, nullptr, FALSE);
            return;
        }
        if (id == Hit_ToggleSmoothCursor) {
            m_config.smoothCursorEnabled = !m_config.smoothCursorEnabled;
            rebakeTimelinePreview();
            updateCurrentComposedBitmap();
            InvalidateRect(m_hwnd, nullptr, FALSE);
            return;
        }
        if (id == Hit_CursorScale_100 || id == Hit_CursorScale_135 || id == Hit_CursorScale_170) {
            m_config.cursorScale = (id == Hit_CursorScale_100) ? 1.00f
                : ((id == Hit_CursorScale_135) ? 1.35f : 1.70f);
            m_config.smoothCursorEnabled = true;
            updateCurrentComposedBitmap();
            InvalidateRect(m_hwnd, nullptr, FALSE);
            return;
        }
        if (id == Hit_ToggleAutoFrame) {
            m_config.autoFrameEnabled = !m_config.autoFrameEnabled;
            updateCurrentComposedBitmap();
            InvalidateRect(m_hwnd, nullptr, FALSE);
            return;
        }
        if (id >= Hit_ThemeBase && id < Hit_ThemeBase + 5) {
            static constexpr std::array<BeautyBackgroundType, 5> kThemes = {{
                BeautyBackgroundType::StudioSlate,
                BeautyBackgroundType::PaperChalk,
                BeautyBackgroundType::SilkMist,
                BeautyBackgroundType::MidnightGraphite,
                BeautyBackgroundType::PureMinimal,
            }};
            m_config.backgroundTheme = kThemes[static_cast<size_t>(id - Hit_ThemeBase)];
            m_config.autoFrameEnabled = true;
            updateCurrentComposedBitmap();
            InvalidateRect(m_hwnd, nullptr, FALSE);
            return;
        }
        if (id == Hit_ToggleCaptions) {
            m_config.autoCaptionsEnabled = !m_config.autoCaptionsEnabled;
            rebakeTimelinePreview();
            updateCurrentComposedBitmap();
            InvalidateRect(m_hwnd, nullptr, FALSE);
            return;
        }
        if (id >= Hit_ZoomSegmentBase) {
            const int idx = it->payload;
            if (idx >= 0 && idx < static_cast<int>(m_timeline.zoomSegments.size())) {
                m_timeline.zoomSegments[static_cast<size_t>(idx)].enabled =
                    !m_timeline.zoomSegments[static_cast<size_t>(idx)].enabled;
                m_playheadSec = std::clamp(
                    m_timeline.zoomSegments[static_cast<size_t>(idx)].startMs / 1000.0 + 0.15,
                    0.0, m_durationSec);
                rebakeTimelinePreview();
                updateCurrentComposedBitmap();
                InvalidateRect(m_hwnd, nullptr, FALSE);
            }
            return;
        }
    }

    // 顶部标题栏空白区域拖拽移动窗口
    if (ly <= 48.0f && m_hwnd) {
        m_draggingWindow = true;
        SetCapture(m_hwnd);
        GetCursorPos(&m_dragStartScreen);
        RECT wr{};
        GetWindowRect(m_hwnd, &wr);
        m_windowStartPos = {wr.left, wr.top};
    }
}

void StudioPreviewWindow::handleMouseMove(float lx, float ly, bool leftButtonDown) {
    if (m_draggingWindow && leftButtonDown && m_hwnd) {
        POINT cur{};
        GetCursorPos(&cur);
        SetWindowPos(
            m_hwnd, nullptr,
            m_windowStartPos.x + (cur.x - m_dragStartScreen.x),
            m_windowStartPos.y + (cur.y - m_dragStartScreen.y),
            0, 0, SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
        return;
    }

    if (m_scrubbingTimeline && leftButtonDown) {
        const float w = (std::max)(1.0f, m_timelineTrackRect.right - m_timelineTrackRect.left);
        const double ratio = std::clamp(static_cast<double>((lx - m_timelineTrackRect.left) / w), 0.0, 1.0);
        m_playheadSec = ratio * m_durationSec;
        updateCurrentComposedBitmap();
        InvalidateRect(m_hwnd, nullptr, FALSE);
        return;
    }

    int newHover = Hit_None;
    int newZoomHover = -1;
    for (auto it = m_hitZones.rbegin(); it != m_hitZones.rend(); ++it) {
        if (pointInRectF(it->rect, lx, ly)) {
            newHover = it->id;
            if (it->id >= Hit_ZoomSegmentBase) {
                newZoomHover = it->payload;
            }
            break;
        }
    }
    if (newHover != m_hoveredHitId || newZoomHover != m_hoveredZoomIdx) {
        m_hoveredHitId = newHover;
        m_hoveredZoomIdx = newZoomHover;
        InvalidateRect(m_hwnd, nullptr, FALSE);
    }
}

void StudioPreviewWindow::handleMouseUp() {
    if (m_scrubbingTimeline || m_draggingWindow) {
        m_scrubbingTimeline = false;
        m_draggingWindow = false;
        ReleaseCapture();
    }
}

LRESULT CALLBACK StudioPreviewWindow::wndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    if (tools3000::ui::KeyboardPipeline::filterWindowMessage(hwnd, msg, wParam, lParam)) {
        return 0;
    }

    StudioPreviewWindow* self = nullptr;
    if (msg == WM_NCCREATE) {
        auto* cs = reinterpret_cast<CREATESTRUCTW*>(lParam);
        self = static_cast<StudioPreviewWindow*>(cs->lpCreateParams);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
    } else {
        self = reinterpret_cast<StudioPreviewWindow*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    }
    if (!self) return DefWindowProcW(hwnd, msg, wParam, lParam);

    switch (msg) {
        case WM_ERASEBKGND:
            return 1;
        case WM_PAINT: {
            PAINTSTRUCT ps{};
            BeginPaint(hwnd, &ps);
            self->render();
            EndPaint(hwnd, &ps);
            return 0;
        }
        case WM_TIMER:
            if (wParam == kPlaybackTimerId) {
                self->onPlaybackTick();
                return 0;
            }
            break;
        case WM_DPICHANGED: {
            self->m_dpiScale = tools3000::core::dpi::scaleForDpi(HIWORD(wParam));
            const auto* suggested = reinterpret_cast<const RECT*>(lParam);
            if (suggested) {
                const int w = suggested->right - suggested->left;
                const int h = suggested->bottom - suggested->top;
                SetWindowPos(hwnd, nullptr, suggested->left, suggested->top, w, h, SWP_NOZORDER | SWP_NOACTIVATE);
                tools3000::core::WinUtils::applyUniversalRoundedCorners(
                    hwnd, w, h, tools3000::core::dpi::scaleMetric(14, self->m_dpiScale));
            }
            self->discardDeviceResources();
            InvalidateRect(hwnd, nullptr, FALSE);
            return 0;
        }
        case WM_SIZE: {
            if (self->m_renderTarget) {
                self->m_renderTarget->Resize(D2D1::SizeU(LOWORD(lParam), HIWORD(lParam)));
            }
            return 0;
        }
        case WM_LBUTTONDOWN: {
            const float s = (std::max)(1.0f, self->m_dpiScale);
            self->handleMouseDown(GET_X_LPARAM(lParam) / s, GET_Y_LPARAM(lParam) / s);
            return 0;
        }
        case WM_MOUSEMOVE: {
            const float s = (std::max)(1.0f, self->m_dpiScale);
            self->handleMouseMove(
                GET_X_LPARAM(lParam) / s,
                GET_Y_LPARAM(lParam) / s,
                (wParam & MK_LBUTTON) != 0);
            SetCursor(LoadCursorW(nullptr, self->m_hoveredHitId != Hit_None ? IDC_HAND : IDC_ARROW));
            return 0;
        }
        case WM_LBUTTONUP:
            self->handleMouseUp();
            return 0;
        case WM_KEYDOWN:
            if (wParam == VK_SPACE && !self->m_exporting.load(std::memory_order_acquire)) {
                self->m_playing = !self->m_playing;
                self->m_lastTickMs = GetTickCount();
                InvalidateRect(hwnd, nullptr, FALSE);
                return 0;
            }
            if (wParam == VK_ESCAPE && !self->m_exporting.load(std::memory_order_acquire)) {
                self->keepOriginalAndClose();
                return 0;
            }
            break;
        case WM_CLOSE:
            if (!self->m_exporting.load(std::memory_order_acquire)) {
                self->keepOriginalAndClose();
            }
            return 0;
        default:
            break;
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

} // namespace tools3000::capture
