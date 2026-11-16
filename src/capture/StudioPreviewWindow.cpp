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
#include <shobjidl.h>
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
    Hit_ThemeBase = 40, // 40..47 对应 8 款高定壁纸主题 (沉稳3 + 轻盈3 + 风景2)
    Hit_ToggleCaptions = 50,
    Hit_OpenExportModalBtn = 60,
    Hit_KeepOriginalBtn = 61,

    // 导出模态窗专属交互热区
    Hit_ModalBackdrop = 200,
    Hit_ModalCardBody = 201,
    Hit_ModalCloseBtn = 202,
    Hit_ModalNameInput = 203,
    Hit_ModalBrowseDirBtn = 204,
    Hit_ModalFormatSelect = 205,
    Hit_ModalResSelect = 206,
    Hit_ModalFpsSelect = 207,
    Hit_ModalQualitySelect = 208,
    Hit_ModalSavePrefsCheck = 209,
    Hit_ModalConfirmExportBtn = 210,
    Hit_ModalDropdownItemBase = 300, // 300 + optionIndex

    Hit_ZoomSegmentBase = 1000, // 1000 + idx 切换单个关键帧放大块启停
};

static constexpr std::array<BeautyBackgroundType, 8> kOrderedStudioThemes = {{
    BeautyBackgroundType::MidnightGraphite, // [沉稳] 深海曜石
    BeautyBackgroundType::StudioSlate,      // [沉稳] 暮夜星云
    BeautyBackgroundType::SilkMist,         // [沉稳] 松烟墨翠
    BeautyBackgroundType::PureMinimal,      // [轻盈] 晨雾云母
    BeautyBackgroundType::PaperChalk,       // [轻盈] 暖杏羊绒
    BeautyBackgroundType::AiryGlacier,      // [轻盈] 晴空薄荷
    BeautyBackgroundType::DuskMountains,    // [风景] 暮色苍山
    BeautyBackgroundType::MistyLakeDawn,    // [风景] 拂晓湖山
}};

const wchar_t* themeDisplayName(BeautyBackgroundType t, bool isZh) noexcept {
    switch (t) {
        case BeautyBackgroundType::MidnightGraphite: return isZh ? L"沉稳 · 深海曜石" : L"Calm · Nordic Obsidian";
        case BeautyBackgroundType::StudioSlate:      return isZh ? L"沉稳 · 暮夜星云" : L"Calm · Velvet Dusk";
        case BeautyBackgroundType::SilkMist:         return isZh ? L"沉稳 · 松烟墨翠" : L"Calm · Pine & Emerald";
        case BeautyBackgroundType::PureMinimal:      return isZh ? L"轻盈 · 晨雾云母" : L"Airy · Cloud Mist";
        case BeautyBackgroundType::PaperChalk:       return isZh ? L"轻盈 · 暖杏羊绒" : L"Airy · Warm Cashmere";
        case BeautyBackgroundType::AiryGlacier:      return isZh ? L"轻盈 · 晴空薄荷" : L"Airy · Glacier Mint";
        case BeautyBackgroundType::DuskMountains:    return isZh ? L"风景 · 暮色苍山" : L"Landscape · Dusk Peaks";
        case BeautyBackgroundType::MistyLakeDawn:    return isZh ? L"风景 · 拂晓湖山" : L"Landscape · Misty Lake";
        default:                                     return isZh ? L"沉稳 · 深海曜石" : L"Calm · Nordic Obsidian";
    }
}

std::string themeKeyFromEnum(BeautyBackgroundType t) {
    switch (t) {
        case BeautyBackgroundType::PaperChalk:       return "sunrise_coral";
        case BeautyBackgroundType::SilkMist:         return "emerald_sea";
        case BeautyBackgroundType::MidnightGraphite: return "obsidian_dark";
        case BeautyBackgroundType::PureMinimal:      return "minimal_light";
        case BeautyBackgroundType::AiryGlacier:      return "airy_glacier";
        case BeautyBackgroundType::DuskMountains:    return "dusk_mountains";
        case BeautyBackgroundType::MistyLakeDawn:    return "misty_lake_dawn";
        case BeautyBackgroundType::StudioSlate:
        default:                                     return "aurora_purple";
    }
}

const wchar_t* formatLabel(StudioExportFormat f, bool isZh) noexcept {
    switch (f) {
        case StudioExportFormat::MP4_H264:      return isZh ? L"MP4 (通用视频)" : L"MP4 (H.264)";
        case StudioExportFormat::MP4_HEVC:      return isZh ? L"MP4 (HEVC 超清)" : L"MP4 (HEVC)";
        case StudioExportFormat::GIF_WeChat:    return isZh ? L"GIF (微信自动播放)" : L"GIF (WeChat Auto-Play)";
        case StudioExportFormat::GIF_HD:        return isZh ? L"GIF (高清文档动图)" : L"GIF (HD Document)";
        case StudioExportFormat::WebP_Animated: return isZh ? L"WebP (网页真彩动图)" : L"WebP (Web Animated)";
        case StudioExportFormat::WebM_VP9:      return isZh ? L"WebM (VP9 视频)" : L"WebM (VP9)";
        default:                                return L"MP4";
    }
}

const wchar_t* resolutionLabel(StudioExportResolution r, bool isZh) noexcept {
    switch (r) {
        case StudioExportResolution::Original: return isZh ? L"原始尺寸" : L"Original";
        case StudioExportResolution::Res2160P: return L"4K (2160P)";
        case StudioExportResolution::Res1440P: return L"2K (1440P)";
        case StudioExportResolution::Res1080P: return L"1080P";
        case StudioExportResolution::Res720P:  return L"720P";
        case StudioExportResolution::Res640P:  return isZh ? L"640P (微信推荐)" : L"640P (WeChat Fit)";
        case StudioExportResolution::Res480P:  return L"480P";
        default:                               return L"1080P";
    }
}

const wchar_t* qualityLabel(StudioExportQuality q, bool isZh) noexcept {
    switch (q) {
        case StudioExportQuality::High:    return isZh ? L"高画质" : L"High Quality";
        case StudioExportQuality::Compact: return isZh ? L"轻量小体积" : L"Compact Size";
        case StudioExportQuality::Recommended:
        default:                           return isZh ? L"推荐" : L"Recommended";
    }
}

std::wstring formatEstimatedSize(uint64_t bytes) {
    wchar_t buf[64]{};
    if (bytes < 1024ULL * 1024ULL) {
        std::swprintf(buf, std::size(buf), L"%u KB",
                      static_cast<unsigned>((std::max<uint64_t>)(1ULL, (bytes + 512ULL) / 1024ULL)));
    } else {
        std::swprintf(buf, std::size(buf), L"%.1f MB",
                      static_cast<double>(bytes) / (1024.0 * 1024.0));
    }
    return buf;
}

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

bool isValidFileNameChar(wchar_t ch) noexcept {
    if (ch < 32) return false;
    switch (ch) {
        case L'\\': case L'/': case L':': case L'*':
        case L'?':  case L'"': case L'<': case L'>': case L'|':
            return false;
        default:
            return true;
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

void drawChevronDown(ID2D1RenderTarget* rt, ID2D1Brush* brush, float cx, float cy) {
    rt->DrawLine(D2D1::Point2F(cx - 4.5f, cy - 2.0f), D2D1::Point2F(cx, cy + 2.5f), brush, 1.6f);
    rt->DrawLine(D2D1::Point2F(cx, cy + 2.5f), D2D1::Point2F(cx + 4.5f, cy - 2.0f), brush, 1.6f);
}

void drawFolderIcon(ID2D1RenderTarget* rt, ID2D1Brush* brush, float cx, float cy) {
    const D2D1_RECT_F body = D2D1::RectF(cx - 7.5f, cy - 4.0f, cx + 7.5f, cy + 6.0f);
    rt->DrawRoundedRectangle(D2D1::RoundedRect(body, 1.8f, 1.8f), brush, 1.4f);
    rt->DrawLine(D2D1::Point2F(cx - 7.0f, cy - 4.0f), D2D1::Point2F(cx - 5.5f, cy - 6.2f), brush, 1.4f);
    rt->DrawLine(D2D1::Point2F(cx - 5.5f, cy - 6.2f), D2D1::Point2F(cx - 1.0f, cy - 6.2f), brush, 1.4f);
    rt->DrawLine(D2D1::Point2F(cx - 1.0f, cy - 6.2f), D2D1::Point2F(cx + 0.8f, cy - 4.0f), brush, 1.4f);
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

void StudioPreviewWindow::initExportModalDefaults() {
    m_exportModalOpen = false;
    m_editingExportName = false;
    m_openDropdown = ExportDropdownKind::None;

    const std::filesystem::path rawFsPath(tools3000::core::WinUtils::utf8ToWstring(m_rawVideoPath));
    m_exportFileNameStem = rawFsPath.stem().wstring();
    if (m_exportFileNameStem.empty()) {
        m_exportFileNameStem = L"Tools3000_Studio";
    }
    const std::wstring rawParent = rawFsPath.parent_path().wstring();

    auto& cm = tools3000::core::ConfigManager::instance();
    m_saveExportPrefs = cm.get<bool>("/recording/studioExport/savePrefs", true);
    const std::string savedDirUtf8 = cm.get<std::string>("/recording/studioExport/directory", "");
    if (!savedDirUtf8.empty()) {
        const std::wstring savedDirW = tools3000::core::WinUtils::utf8ToWstring(savedDirUtf8);
        std::error_code ec;
        if (std::filesystem::is_directory(savedDirW, ec) && !ec) {
            m_exportDirectoryWide = savedDirW;
        }
    }
    if (m_exportDirectoryWide.empty()) {
        m_exportDirectoryWide = rawParent;
    }

    const int fmtInt = std::clamp(cm.get<int>("/recording/studioExport/format", 0), 0, 5);
    const int resInt = std::clamp(cm.get<int>("/recording/studioExport/resolution", 3), 0, 6);
    const int qualInt = std::clamp(cm.get<int>("/recording/studioExport/quality", 0), 0, 2);
    const int savedFps = cm.get<int>("/recording/studioExport/fps", 30);

    m_exportOpts.format = static_cast<StudioExportFormat>(fmtInt);
    m_exportOpts.resolution = static_cast<StudioExportResolution>(resInt);
    m_exportOpts.quality = static_cast<StudioExportQuality>(qualInt);
    m_exportOpts.fps = std::clamp(savedFps, 10, 60);
    m_exportOpts.baseBitrateMbps = m_bitrateMbps;

    if (m_exportOpts.format == StudioExportFormat::GIF_WeChat ||
        m_exportOpts.format == StudioExportFormat::GIF_HD) {
        if (m_exportOpts.fps > 15) m_exportOpts.fps = 12;
    }
}

void StudioPreviewWindow::applyFormatSmartDefaults(StudioExportFormat newFmt) {
    m_exportOpts.format = newFmt;
    if (newFmt == StudioExportFormat::GIF_WeChat) {
        m_exportOpts.resolution = StudioExportResolution::Res640P;
        m_exportOpts.fps = 12;
        m_exportOpts.quality = StudioExportQuality::Recommended;
    } else if (newFmt == StudioExportFormat::GIF_HD) {
        m_exportOpts.resolution = StudioExportResolution::Res720P;
        m_exportOpts.fps = 15;
    } else if (newFmt == StudioExportFormat::WebP_Animated) {
        if (m_exportOpts.resolution == StudioExportResolution::Res640P ||
            m_exportOpts.resolution == StudioExportResolution::Res480P) {
            m_exportOpts.resolution = StudioExportResolution::Res1080P;
        }
        m_exportOpts.fps = 24;
    } else {
        if (m_exportOpts.resolution == StudioExportResolution::Res640P ||
            m_exportOpts.resolution == StudioExportResolution::Res480P) {
            m_exportOpts.resolution = StudioExportResolution::Res1080P;
        }
        if (m_exportOpts.fps < 24) {
            m_exportOpts.fps = 30;
        }
    }
}

void StudioPreviewWindow::persistExportPreferencesIfChecked() {
    auto& cm = tools3000::core::ConfigManager::instance();
    cm.set<bool>("/recording/studioExport/savePrefs", m_saveExportPrefs);
    if (!m_saveExportPrefs) return;

    cm.set<int>("/recording/studioExport/format", static_cast<int>(m_exportOpts.format));
    cm.set<int>("/recording/studioExport/resolution", static_cast<int>(m_exportOpts.resolution));
    cm.set<int>("/recording/studioExport/fps", m_exportOpts.fps);
    cm.set<int>("/recording/studioExport/quality", static_cast<int>(m_exportOpts.quality));
    if (!m_exportDirectoryWide.empty()) {
        cm.set<std::string>("/recording/studioExport/directory",
                            tools3000::core::WinUtils::wstringToUtf8(m_exportDirectoryWide));
    }
}

std::string StudioPreviewWindow::buildFinalExportPathUtf8() const {
    std::wstring cleanStem;
    for (wchar_t ch : m_exportFileNameStem) {
        if (isValidFileNameChar(ch)) cleanStem.push_back(ch);
    }
    while (!cleanStem.empty() && (cleanStem.back() == L' ' || cleanStem.back() == L'.')) {
        cleanStem.pop_back();
    }
    if (cleanStem.empty()) {
        cleanStem = L"Tools3000_Studio";
    }

    const std::wstring extW = tools3000::core::WinUtils::utf8ToWstring(
        StudioVideoCompositor::extensionForExportFormat(m_exportOpts.format));
    std::filesystem::path dirPath = m_exportDirectoryWide.empty()
        ? std::filesystem::path(tools3000::core::WinUtils::utf8ToWstring(m_rawVideoPath)).parent_path()
        : std::filesystem::path(m_exportDirectoryWide);
    const std::filesystem::path fullPath = dirPath / (cleanStem + extW);
    return tools3000::core::WinUtils::wstringToUtf8(fullPath.wstring());
}

void StudioPreviewWindow::browseExportDirectory() {
    ComPtr<IFileOpenDialog> dlg;
    if (FAILED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(dlg.GetAddressOf()))) || !dlg) {
        return;
    }
    DWORD fos = 0;
    if (SUCCEEDED(dlg->GetOptions(&fos))) {
        dlg->SetOptions(fos | FOS_PICKFOLDERS | FOS_FORCEFILESYSTEM | FOS_NOCHANGEDIR);
    }
    if (!m_exportDirectoryWide.empty()) {
        ComPtr<IShellItem> initFolder;
        if (SUCCEEDED(SHCreateItemFromParsingName(m_exportDirectoryWide.c_str(), nullptr, IID_PPV_ARGS(initFolder.GetAddressOf()))) && initFolder) {
            dlg->SetFolder(initFolder.Get());
        }
    }
    if (SUCCEEDED(dlg->Show(m_hwnd))) {
        ComPtr<IShellItem> resItem;
        if (SUCCEEDED(dlg->GetResult(resItem.GetAddressOf())) && resItem) {
            PWSTR rawPath = nullptr;
            if (SUCCEEDED(resItem->GetDisplayName(SIGDN_FILESYSPATH, &rawPath)) && rawPath) {
                m_exportDirectoryWide = rawPath;
                CoTaskMemFree(rawPath);
                InvalidateRect(m_hwnd, nullptr, FALSE);
            }
        }
    }
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

    initExportModalDefaults();

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

        ComPtr<IDWriteInlineObject> ellipsis;
        if (m_bodyFmt && SUCCEEDED(m_dwriteFactory->CreateEllipsisTrimmingSign(m_bodyFmt.Get(), ellipsis.GetAddressOf()))) {
            DWRITE_TRIMMING trim{DWRITE_TRIMMING_GRANULARITY_CHARACTER, 0, 0};
            m_bodyFmt->SetTrimming(&trim, ellipsis.Get());
            m_bodyFmt->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
        }
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

    // 预生成 8 款高定壁纸（沉稳 / 轻盈 / 风景）的真实迷你缩略预览位图
    if (!m_themeThumbBitmaps[0] && m_renderTarget) {
        D2D1_BITMAP_PROPERTIES bp = D2D1::BitmapProperties(
            D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_IGNORE),
            96.0f, 96.0f);
        for (size_t i = 0; i < kOrderedStudioThemes.size(); ++i) {
            cv::Mat thumb = StudioVideoCompositor::renderThemeThumbnailBgra(kOrderedStudioThemes[i], 96, 56);
            if (!thumb.empty()) {
                m_renderTarget->CreateBitmap(
                    D2D1::SizeU(static_cast<UINT32>(thumb.cols), static_cast<UINT32>(thumb.rows)),
                    thumb.data,
                    static_cast<UINT32>(thumb.step[0]),
                    &bp,
                    m_themeThumbBitmaps[i].GetAddressOf());
            }
        }
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
    for (auto& bmp : m_themeThumbBitmaps) bmp.Reset();
    m_frameBitmap.Reset();
    m_bitmapW = 0;
    m_bitmapH = 0;
    m_renderTarget.Reset();
    m_composedDirty = true;
}

void StudioPreviewWindow::renderExportModalOverlay(float W, float H, bool dark, bool isZh, ID2D1SolidColorBrush* brush) {
    // 导出模态窗打开时，清空底层画布热区以实施模态焦点隔离
    m_hitZones.clear();

    const auto accentRgb = tools3000::core::NativeThemePalette::getEffectiveAccentColorRGB();
    const D2D1_COLOR_F colAccent = D2D1::ColorF(accentRgb.r, accentRgb.g, accentRgb.b, 1.0f);
    const D2D1_COLOR_F colModalBg = dark
        ? D2D1::ColorF(0.082f, 0.094f, 0.133f, 0.985f)  // 深色微晶黑曜底
        : D2D1::ColorF(0.992f, 0.995f, 1.000f, 0.990f); // 浅色雪瓷白底
    const D2D1_COLOR_F colFieldBg = dark
        ? D2D1::ColorF(0.118f, 0.137f, 0.192f, 1.0f)
        : D2D1::ColorF(0.949f, 0.961f, 0.980f, 1.0f);
    const D2D1_COLOR_F colBorder = dark
        ? D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.14f)
        : D2D1::ColorF(0.059f, 0.090f, 0.165f, 0.14f);
    const D2D1_COLOR_F colPrimary = dark
        ? D2D1::ColorF(0.96f, 0.97f, 0.99f, 1.0f)
        : D2D1::ColorF(0.059f, 0.090f, 0.165f, 1.0f);
    const D2D1_COLOR_F colSecondary = dark
        ? D2D1::ColorF(0.64f, 0.70f, 0.80f, 1.0f)
        : D2D1::ColorF(0.35f, 0.41f, 0.51f, 1.0f);

    auto setBrush = [&](const D2D1_COLOR_F& c) { brush->SetColor(c); };
    auto drawTxt = [&](const std::wstring& txt, IDWriteTextFormat* fmt, const D2D1_RECT_F& r,
                       const D2D1_COLOR_F& col,
                       DWRITE_TEXT_ALIGNMENT hAlign = DWRITE_TEXT_ALIGNMENT_LEADING) {
        if (!fmt || txt.empty()) return;
        fmt->SetTextAlignment(hAlign);
        fmt->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
        setBrush(col);
        m_renderTarget->DrawText(txt.c_str(), static_cast<UINT32>(txt.size()), fmt, r, brush);
    };

    const bool exporting = m_exporting.load(std::memory_order_acquire);

    // 1. 全窗暗化遮罩
    const D2D1_RECT_F fullRc = D2D1::RectF(0.0f, 0.0f, W, H);
    if (!exporting) {
        m_hitZones.push_back({Hit_ModalBackdrop, fullRc, -1});
    }
    setBrush(D2D1::ColorF(0.02f, 0.03f, 0.06f, dark ? 0.68f : 0.45f));
    m_renderTarget->FillRectangle(fullRc, brush);

    // 2. 居中导出卡片 (496 x 418)
    const float cardW = 496.0f;
    const float cardH = 418.0f;
    const float cardL = (W - cardW) * 0.5f;
    const float cardT = (H - cardH) * 0.5f;
    const D2D1_RECT_F cardRc = D2D1::RectF(cardL, cardT, cardL + cardW, cardT + cardH);
    m_hitZones.push_back({Hit_ModalCardBody, cardRc, -1});

    setBrush(colModalBg);
    m_renderTarget->FillRoundedRectangle(D2D1::RoundedRect(cardRc, 16.0f, 16.0f), brush);
    setBrush(colBorder);
    m_renderTarget->DrawRoundedRectangle(D2D1::RoundedRect(cardRc, 16.0f, 16.0f), brush, 1.2f);

    // 标题 "导出" + 右上角 [X]
    drawTxt(isZh ? L"导出" : L"Export", m_titleFmt.Get(),
            D2D1::RectF(cardL + 24.0f, cardT + 14.0f, cardRc.right - 24.0f, cardT + 48.0f),
            colPrimary, DWRITE_TEXT_ALIGNMENT_CENTER);

    const D2D1_RECT_F mCloseRc = D2D1::RectF(cardRc.right - 40.0f, cardT + 14.0f, cardRc.right - 14.0f, cardT + 40.0f);
    if (!exporting) {
        m_hitZones.push_back({Hit_ModalCloseBtn, mCloseRc, -1});
    }
    setBrush(m_hoveredHitId == Hit_ModalCloseBtn ? D2D1::ColorF(0.94f, 0.27f, 0.27f, 1.0f) : colSecondary);
    const float mcx = (mCloseRc.left + mCloseRc.right) * 0.5f;
    const float mcy = (mCloseRc.top + mCloseRc.bottom) * 0.5f;
    m_renderTarget->DrawLine({mcx - 4.5f, mcy - 4.5f}, {mcx + 4.5f, mcy + 4.5f}, brush, 1.6f);
    m_renderTarget->DrawLine({mcx + 4.5f, mcy - 4.5f}, {mcx - 4.5f, mcy + 4.5f}, brush, 1.6f);

    const float inL = cardL + 26.0f;
    const float inR = cardRc.right - 26.0f;
    float y = cardT + 56.0f;

    // 3. 名称 (File Name)
    drawTxt(isZh ? L"名称" : L"Name", m_captionFmt.Get(), D2D1::RectF(inL, y, inR, y + 20.0f), colSecondary);
    y += 22.0f;
    const D2D1_RECT_F nameBox = D2D1::RectF(inL, y, inR, y + 38.0f);
    if (!exporting) {
        m_hitZones.push_back({Hit_ModalNameInput, nameBox, -1});
    }
    setBrush(colFieldBg);
    m_renderTarget->FillRoundedRectangle(D2D1::RoundedRect(nameBox, 8.0f, 8.0f), brush);
    setBrush(m_editingExportName ? colAccent : colBorder);
    m_renderTarget->DrawRoundedRectangle(D2D1::RoundedRect(nameBox, 8.0f, 8.0f), brush, m_editingExportName ? 1.5f : 1.0f);

    const std::wstring extHint = tools3000::core::WinUtils::utf8ToWstring(
        StudioVideoCompositor::extensionForExportFormat(m_exportOpts.format));
    std::wstring nameShown = m_exportFileNameStem;
    if (m_editingExportName && ((GetTickCount() / 500) % 2 == 0)) {
        nameShown.push_back(L'|');
    }
    drawTxt(nameShown, m_bodyFmt.Get(), D2D1::RectF(inL + 12.0f, y, inR - 56.0f, y + 38.0f), colPrimary);
    drawTxt(extHint, m_monoFmt.Get(), D2D1::RectF(inR - 54.0f, y, inR - 12.0f, y + 38.0f), colSecondary, DWRITE_TEXT_ALIGNMENT_TRAILING);

    // 4. 保存至 (Save To)
    y += 48.0f;
    drawTxt(isZh ? L"保存至" : L"Save To", m_captionFmt.Get(), D2D1::RectF(inL, y, inR, y + 20.0f), colSecondary);
    y += 22.0f;
    const D2D1_RECT_F dirBox = D2D1::RectF(inL, y, inR, y + 38.0f);
    const D2D1_RECT_F dirBtn = D2D1::RectF(inR - 40.0f, y + 2.0f, inR - 2.0f, y + 36.0f);
    if (!exporting) {
        m_hitZones.push_back({Hit_ModalBrowseDirBtn, dirBox, -1});
    }
    setBrush(colFieldBg);
    m_renderTarget->FillRoundedRectangle(D2D1::RoundedRect(dirBox, 8.0f, 8.0f), brush);
    setBrush(m_hoveredHitId == Hit_ModalBrowseDirBtn ? colAccent : colBorder);
    m_renderTarget->DrawRoundedRectangle(D2D1::RoundedRect(dirBox, 8.0f, 8.0f), brush, 1.0f);
    drawTxt(m_exportDirectoryWide, m_bodyFmt.Get(), D2D1::RectF(inL + 12.0f, y, inR - 42.0f, y + 38.0f), colPrimary);
    setBrush(m_hoveredHitId == Hit_ModalBrowseDirBtn ? colAccent : colSecondary);
    drawFolderIcon(m_renderTarget.Get(), brush, (dirBtn.left + dirBtn.right) * 0.5f, (dirBtn.top + dirBtn.bottom) * 0.5f);

    // 5. 2x2 下拉框矩阵：格式 / 分辨率 / 帧率 / 质量
    y += 50.0f;
    const float colGap = 14.0f;
    const float halfW = (inR - inL - colGap) * 0.5f;
    const float rightColL = inL + halfW + colGap;

    auto drawSelectBox = [&](int hitId, float bx, float by, const std::wstring& title, const std::wstring& val, bool activeOpen) -> D2D1_RECT_F {
        drawTxt(title, m_captionFmt.Get(), D2D1::RectF(bx, by, bx + halfW, by + 20.0f), colSecondary);
        const D2D1_RECT_F box = D2D1::RectF(bx, by + 22.0f, bx + halfW, by + 60.0f);
        if (!exporting) {
            m_hitZones.push_back({hitId, box, -1});
        }
        setBrush(colFieldBg);
        m_renderTarget->FillRoundedRectangle(D2D1::RoundedRect(box, 8.0f, 8.0f), brush);
        setBrush((activeOpen || m_hoveredHitId == hitId) ? colAccent : colBorder);
        m_renderTarget->DrawRoundedRectangle(D2D1::RoundedRect(box, 8.0f, 8.0f), brush, activeOpen ? 1.5f : 1.0f);
        drawTxt(val, m_bodyFmt.Get(), D2D1::RectF(box.left + 12.0f, box.top, box.right - 28.0f, box.bottom), colPrimary);
        setBrush(colSecondary);
        drawChevronDown(m_renderTarget.Get(), brush, box.right - 15.0f, (box.top + box.bottom) * 0.5f);
        return box;
    };

    const D2D1_RECT_F fmtBox = drawSelectBox(
        Hit_ModalFormatSelect, inL, y,
        isZh ? L"格式" : L"Format",
        formatLabel(m_exportOpts.format, isZh),
        m_openDropdown == ExportDropdownKind::Format);
    const D2D1_RECT_F resBox = drawSelectBox(
        Hit_ModalResSelect, rightColL, y,
        isZh ? L"分辨率" : L"Resolution",
        resolutionLabel(m_exportOpts.resolution, isZh),
        m_openDropdown == ExportDropdownKind::Resolution);

    y += 70.0f;
    wchar_t fpsTxt[32]{};
    std::swprintf(fpsTxt, std::size(fpsTxt), L"%d FPS", m_exportOpts.fps);
    const D2D1_RECT_F fpsBox = drawSelectBox(
        Hit_ModalFpsSelect, inL, y,
        isZh ? L"帧率" : L"Frame Rate",
        fpsTxt,
        m_openDropdown == ExportDropdownKind::Fps);
    const D2D1_RECT_F qualBox = drawSelectBox(
        Hit_ModalQualitySelect, rightColL, y,
        isZh ? L"质量" : L"Quality",
        qualityLabel(m_exportOpts.quality, isZh),
        m_openDropdown == ExportDropdownKind::Quality);

    // 6. 主导出按钮
    y += 74.0f;
    const D2D1_RECT_F confirmBtn = D2D1::RectF(inL, y, inR, y + 42.0f);
    if (!exporting) {
        m_hitZones.push_back({Hit_ModalConfirmExportBtn, confirmBtn, -1});
    }

    ComPtr<ID2D1LinearGradientBrush> gradBrush;
    D2D1_GRADIENT_STOP stops[2] = {
        {0.0f, D2D1::ColorF(0.24f, 0.44f, 0.98f, exporting ? 0.40f : 1.0f)},
        {1.0f, D2D1::ColorF(0.58f, 0.36f, 0.98f, exporting ? 0.40f : 1.0f)},
    };
    ComPtr<ID2D1GradientStopCollection> stopColl;
    if (SUCCEEDED(m_renderTarget->CreateGradientStopCollection(stops, 2, stopColl.GetAddressOf())) && stopColl) {
        m_renderTarget->CreateLinearGradientBrush(
            D2D1::LinearGradientBrushProperties({confirmBtn.left, confirmBtn.top}, {confirmBtn.right, confirmBtn.bottom}),
            stopColl.Get(),
            gradBrush.GetAddressOf());
    }
    if (gradBrush) {
        m_renderTarget->FillRoundedRectangle(D2D1::RoundedRect(confirmBtn, 10.0f, 10.0f), gradBrush.Get());
    } else {
        setBrush(colAccent);
        m_renderTarget->FillRoundedRectangle(D2D1::RoundedRect(confirmBtn, 10.0f, 10.0f), brush);
    }

    if (exporting) {
        const double prog = std::clamp(m_exportProgress.load(std::memory_order_relaxed), 0.0, 1.0);
        const float fillW = (confirmBtn.right - confirmBtn.left) * static_cast<float>(prog);
        setBrush(colAccent);
        m_renderTarget->FillRoundedRectangle(
            D2D1::RoundedRect(D2D1::RectF(confirmBtn.left, confirmBtn.top, confirmBtn.left + fillW, confirmBtn.bottom), 10.0f, 10.0f),
            brush);
        wchar_t pTxt[80]{};
        std::swprintf(pTxt, std::size(pTxt),
                      isZh ? L"正在合成导出 %d%%..." : L"Exporting %d%%...",
                      static_cast<int>(std::round(prog * 100.0)));
        drawTxt(pTxt, m_sectionFmt.Get(), confirmBtn, D2D1::ColorF(1.0f, 1.0f, 1.0f, 1.0f), DWRITE_TEXT_ALIGNMENT_CENTER);
    } else {
        drawTxt(isZh ? L"导出" : L"Export", m_sectionFmt.Get(), confirmBtn,
                D2D1::ColorF(1.0f, 1.0f, 1.0f, 1.0f), DWRITE_TEXT_ALIGNMENT_CENTER);
    }

    // 7. 底栏：左 [✓] 保存当前设置 + 右 (预计大小: ~X.X MB)
    y += 50.0f;
    const D2D1_RECT_F chkHit = D2D1::RectF(inL, y - 2.0f, inL + 150.0f, y + 22.0f);
    if (!exporting) {
        m_hitZones.push_back({Hit_ModalSavePrefsCheck, chkHit, -1});
    }
    const D2D1_RECT_F chkBox = D2D1::RectF(inL, y + 2.0f, inL + 16.0f, y + 18.0f);
    if (m_saveExportPrefs) {
        setBrush(colAccent);
        m_renderTarget->FillRoundedRectangle(D2D1::RoundedRect(chkBox, 4.0f, 4.0f), brush);
        setBrush(D2D1::ColorF(1.0f, 1.0f, 1.0f, 1.0f));
        m_renderTarget->DrawLine({chkBox.left + 3.5f, chkBox.top + 8.5f}, {chkBox.left + 6.8f, chkBox.top + 11.8f}, brush, 1.8f);
        m_renderTarget->DrawLine({chkBox.left + 6.8f, chkBox.top + 11.8f}, {chkBox.left + 12.8f, chkBox.top + 4.8f}, brush, 1.8f);
    } else {
        setBrush(colBorder);
        m_renderTarget->DrawRoundedRectangle(D2D1::RoundedRect(chkBox, 4.0f, 4.0f), brush, 1.2f);
    }
    drawTxt(isZh ? L"保存当前设置" : L"Save current settings", m_captionFmt.Get(),
            D2D1::RectF(chkBox.right + 8.0f, y - 2.0f, inL + 170.0f, y + 22.0f), colSecondary);

    const uint64_t estBytes = StudioVideoCompositor::estimateExportSizeBytes(
        m_origWidth, m_origHeight, m_durationSec, m_exportOpts);
    const std::wstring estStr = formatEstimatedSize(estBytes);
    std::wstring sizeInfo;
    if (m_exportOpts.format == StudioExportFormat::GIF_WeChat) {
        sizeInfo = isZh
            ? (L"(预计大小: ~" + estStr + L" · 微信自动播放)")
            : (L"(Est: ~" + estStr + L" · WeChat Auto-Play)");
    } else {
        sizeInfo = isZh
            ? (L"(预计大小: ~" + estStr + L")")
            : (L"(Est. size: ~" + estStr + L")");
    }
    drawTxt(sizeInfo, m_captionFmt.Get(),
            D2D1::RectF(inL + 165.0f, y - 2.0f, inR, y + 22.0f),
            m_exportOpts.format == StudioExportFormat::GIF_WeChat ? colAccent : colSecondary,
            DWRITE_TEXT_ALIGNMENT_TRAILING);

    // 8. 展开中的下拉浮层菜单（最后绘制以置于最顶层）
    if (m_openDropdown != ExportDropdownKind::None && !exporting) {
        struct DropItem {
            int value;
            std::wstring label;
            bool selected;
        };
        std::vector<DropItem> items;
        D2D1_RECT_F anchor{};

        if (m_openDropdown == ExportDropdownKind::Format) {
            anchor = fmtBox;
            for (int v = 0; v <= 5; ++v) {
                const auto f = static_cast<StudioExportFormat>(v);
                items.push_back({v, formatLabel(f, isZh), m_exportOpts.format == f});
            }
        } else if (m_openDropdown == ExportDropdownKind::Resolution) {
            anchor = resBox;
            for (int v = 0; v <= 6; ++v) {
                const auto r = static_cast<StudioExportResolution>(v);
                items.push_back({v, resolutionLabel(r, isZh), m_exportOpts.resolution == r});
            }
        } else if (m_openDropdown == ExportDropdownKind::Fps) {
            anchor = fpsBox;
            const bool isGif = (m_exportOpts.format == StudioExportFormat::GIF_WeChat ||
                                m_exportOpts.format == StudioExportFormat::GIF_HD);
            if (isGif) {
                for (int f : {15, 12, 10}) {
                    items.push_back({f, std::to_wstring(f) + L" FPS", m_exportOpts.fps == f});
                }
            } else {
                for (int f : {60, 30, 24, 15}) {
                    items.push_back({f, std::to_wstring(f) + L" FPS", m_exportOpts.fps == f});
                }
            }
        } else if (m_openDropdown == ExportDropdownKind::Quality) {
            anchor = qualBox;
            for (int v = 0; v <= 2; ++v) {
                const auto q = static_cast<StudioExportQuality>(v);
                items.push_back({v, qualityLabel(q, isZh), m_exportOpts.quality == q});
            }
        }

        if (!items.empty()) {
            const float rowH = 30.0f;
            const float menuH = static_cast<float>(items.size()) * rowH + 8.0f;
            float menuTop = anchor.bottom + 4.0f;
            if (menuTop + menuH > H - 10.0f) {
                menuTop = anchor.top - menuH - 4.0f;
            }
            const D2D1_RECT_F menuRc = D2D1::RectF(anchor.left, menuTop, anchor.right, menuTop + menuH);
            setBrush(dark ? D2D1::ColorF(0.105f, 0.125f, 0.180f, 0.995f) : D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.995f));
            m_renderTarget->FillRoundedRectangle(D2D1::RoundedRect(menuRc, 8.0f, 8.0f), brush);
            setBrush(colAccent);
            m_renderTarget->DrawRoundedRectangle(D2D1::RoundedRect(menuRc, 8.0f, 8.0f), brush, 1.1f);

            for (size_t i = 0; i < items.size(); ++i) {
                const D2D1_RECT_F itemRc = D2D1::RectF(
                    menuRc.left + 4.0f,
                    menuTop + 4.0f + static_cast<float>(i) * rowH,
                    menuRc.right - 4.0f,
                    menuTop + 4.0f + static_cast<float>(i + 1) * rowH);
                const int hId = Hit_ModalDropdownItemBase + static_cast<int>(i);
                m_hitZones.push_back({hId, itemRc, items[i].value});
                if (items[i].selected || m_hoveredHitId == hId) {
                    setBrush(D2D1::ColorF(accentRgb.r, accentRgb.g, accentRgb.b, items[i].selected ? 0.22f : 0.12f));
                    m_renderTarget->FillRoundedRectangle(D2D1::RoundedRect(itemRc, 5.0f, 5.0f), brush);
                }
                drawTxt(items[i].label, m_bodyFmt.Get(),
                        D2D1::RectF(itemRc.left + 10.0f, itemRc.top, itemRc.right - 8.0f, itemRc.bottom),
                        items[i].selected ? colAccent : colPrimary);
            }
        }
    }
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
        ? D2D1::ColorF(0.059f, 0.075f, 0.110f, 1.0f)
        : D2D1::ColorF(0.969f, 0.976f, 0.988f, 1.0f);
    const D2D1_COLOR_F colCardBg = dark
        ? D2D1::ColorF(0.094f, 0.118f, 0.169f, 1.0f)
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

    const D2D1_RECT_F closeRc = D2D1::RectF(W - 44.0f, 10.0f, W - 16.0f, 38.0f);
    m_hitZones.push_back({Hit_CloseBtn, closeRc, -1});
    if (m_hoveredHitId == Hit_CloseBtn && !m_exportModalOpen) {
        setBrush(D2D1::ColorF(0.937f, 0.267f, 0.267f, 0.18f));
        m_renderTarget->FillRoundedRectangle(D2D1::RoundedRect(closeRc, 7.0f, 7.0f), brush.Get());
    }
    setBrush((m_hoveredHitId == Hit_CloseBtn && !m_exportModalOpen) ? D2D1::ColorF(0.937f, 0.267f, 0.267f, 1.0f) : colSecondary);
    const float cx = (closeRc.left + closeRc.right) * 0.5f;
    const float cy = (closeRc.top + closeRc.bottom) * 0.5f;
    m_renderTarget->DrawLine(D2D1::Point2F(cx - 5.0f, cy - 5.0f), D2D1::Point2F(cx + 5.0f, cy + 5.0f), brush.Get(), 1.8f);
    m_renderTarget->DrawLine(D2D1::Point2F(cx + 5.0f, cy - 5.0f), D2D1::Point2F(cx - 5.0f, cy + 5.0f), brush.Get(), 1.8f);

    setBrush(colBorder);
    m_renderTarget->DrawLine(D2D1::Point2F(0.0f, headerH), D2D1::Point2F(W, headerH), brush.Get(), 1.0f);

    const float rightPanelW = 312.0f;
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

    // 3. 左下角双轨可视化时间轴卡片
    const D2D1_RECT_F tlCard = D2D1::RectF(leftAreaLeft, stageBottom + 10.0f, leftAreaRight, H - 16.0f);
    setBrush(colCardBg);
    m_renderTarget->FillRoundedRectangle(D2D1::RoundedRect(tlCard, 12.0f, 12.0f), brush.Get());
    setBrush(colBorder);
    m_renderTarget->DrawRoundedRectangle(D2D1::RoundedRect(tlCard, 12.0f, 12.0f), brush.Get(), 1.0f);

    const D2D1_RECT_F playBtn = D2D1::RectF(tlCard.left + 14.0f, tlCard.top + 12.0f, tlCard.left + 44.0f, tlCard.top + 42.0f);
    m_hitZones.push_back({Hit_PlayPauseBtn, playBtn, -1});
    setBrush(m_hoveredHitId == Hit_PlayPauseBtn ? colAccent : colAccentSoft);
    m_renderTarget->FillRoundedRectangle(D2D1::RoundedRect(playBtn, 8.0f, 8.0f), brush.Get());
    setBrush((m_hoveredHitId == Hit_PlayPauseBtn) ? D2D1::ColorF(1.0f, 1.0f, 1.0f, 1.0f) : colAccent);
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

    const std::wstring timeStr = formatTimecodeMmSs(m_playheadSec) + L" / " + formatTimecodeMmSs(m_durationSec);
    drawTextAligned(timeStr, m_monoFmt.Get(),
                    D2D1::RectF(playBtn.right + 10.0f, tlCard.top + 12.0f, playBtn.right + 142.0f, tlCard.top + 42.0f),
                    colPrimary);

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
    float curY = sideCard.top + 14.0f;

    // (A) 自动关键帧放大
    drawTextAligned(isZh ? L"关键帧自动放大" : L"Auto-Zoom Camera", m_sectionFmt.Get(),
                    D2D1::RectF(padL, curY, padR - 48.0f, curY + 22.0f), colPrimary);
    drawTogglePill(Hit_ToggleAutoZoom, padR, curY, m_config.autoZoomEnabled);
    curY += 24.0f;
    wchar_t zoomSub[96]{};
    std::swprintf(zoomSub, std::size(zoomSub),
                  isZh ? L"弹簧平滑跟随点击与输入 (%zu 段)" : L"Spring camera on clicks (%zu segs)",
                  m_timeline.zoomSegments.size());
    drawTextAligned(zoomSub, m_captionFmt.Get(), D2D1::RectF(padL, curY, padR, curY + 18.0f), colSecondary);
    curY += 22.0f;
    {
        const float cw = (padR - padL - 16.0f) / 3.0f;
        drawOptionChip(Hit_ZoomPreset_150, D2D1::RectF(padL, curY, padL + cw, curY + 25.0f),
                       L"1.5x", std::abs(m_config.defaultZoomFactor - 1.50f) < 0.12f);
        drawOptionChip(Hit_ZoomPreset_185, D2D1::RectF(padL + cw + 8.0f, curY, padL + cw * 2.0f + 8.0f, curY + 25.0f),
                       L"1.85x", std::abs(m_config.defaultZoomFactor - 1.85f) < 0.15f);
        drawOptionChip(Hit_ZoomPreset_220, D2D1::RectF(padL + cw * 2.0f + 16.0f, curY, padR, curY + 25.0f),
                       L"2.2x", std::abs(m_config.defaultZoomFactor - 2.20f) < 0.15f);
    }
    curY += 35.0f;
    setBrush(colBorder);
    m_renderTarget->DrawLine(D2D1::Point2F(padL, curY), D2D1::Point2F(padR, curY), brush.Get(), 1.0f);
    curY += 10.0f;

    // (B) 光标平滑移动与超清重绘
    drawTextAligned(isZh ? L"光标平滑与超清重绘" : L"Smooth Vector Cursor", m_sectionFmt.Get(),
                    D2D1::RectF(padL, curY, padR - 48.0f, curY + 22.0f), colPrimary);
    drawTogglePill(Hit_ToggleSmoothCursor, padR, curY, m_config.smoothCursorEnabled);
    curY += 24.0f;
    drawTextAligned(isZh ? L"消除手部微抖动，放大后依然锐利" : L"De-jittered crisp vector pointer",
                    m_captionFmt.Get(), D2D1::RectF(padL, curY, padR, curY + 18.0f), colSecondary);
    curY += 22.0f;
    {
        const float cw = (padR - padL - 16.0f) / 3.0f;
        drawOptionChip(Hit_CursorScale_100, D2D1::RectF(padL, curY, padL + cw, curY + 25.0f),
                       isZh ? L"标准 1.0x" : L"1.0x", std::abs(m_config.cursorScale - 1.00f) < 0.15f);
        drawOptionChip(Hit_CursorScale_135, D2D1::RectF(padL + cw + 8.0f, curY, padL + cw * 2.0f + 8.0f, curY + 25.0f),
                       isZh ? L"清晰 1.35x" : L"1.35x", std::abs(m_config.cursorScale - 1.35f) < 0.15f);
        drawOptionChip(Hit_CursorScale_170, D2D1::RectF(padL + cw * 2.0f + 16.0f, curY, padR, curY + 25.0f),
                       isZh ? L"演示 1.7x" : L"1.7x", std::abs(m_config.cursorScale - 1.70f) < 0.15f);
    }
    curY += 35.0f;
    setBrush(colBorder);
    m_renderTarget->DrawLine(D2D1::Point2F(padL, curY), D2D1::Point2F(padR, curY), brush.Get(), 1.0f);
    curY += 10.0f;

    // (C) 高定艺术背景与微晶圆角画框 (沉稳 / 轻盈 / 风景 8 款真实缩略预览卡)
    drawTextAligned(isZh ? L"艺术背景与微晶画框" : L"Backdrop & Glass Frame", m_sectionFmt.Get(),
                    D2D1::RectF(padL, curY, padR - 48.0f, curY + 22.0f), colPrimary);
    drawTogglePill(Hit_ToggleAutoFrame, padR, curY, m_config.autoFrameEnabled);
    curY += 24.0f;

    BeautyBackgroundType previewNameTheme = m_config.backgroundTheme;
    if (m_hoveredHitId >= Hit_ThemeBase && m_hoveredHitId < Hit_ThemeBase + 8) {
        previewNameTheme = kOrderedStudioThemes[static_cast<size_t>(m_hoveredHitId - Hit_ThemeBase)];
    }
    drawTextAligned(themeDisplayName(previewNameTheme, isZh), m_captionFmt.Get(),
                    D2D1::RectF(padL, curY, padR, curY + 18.0f), colAccent);
    curY += 22.0f;
    {
        const float gapX = 8.0f;
        const float gapY = 8.0f;
        const float thumbW = (padR - padL - gapX * 3.0f) / 4.0f;
        const float thumbH = 34.0f;
        for (size_t i = 0; i < kOrderedStudioThemes.size(); ++i) {
            const size_t col = i % 4;
            const size_t row = i / 4;
            const float tx = padL + static_cast<float>(col) * (thumbW + gapX);
            const float ty = curY + static_cast<float>(row) * (thumbH + gapY);
            const D2D1_RECT_F trc = D2D1::RectF(tx, ty, tx + thumbW, ty + thumbH);
            const int hId = Hit_ThemeBase + static_cast<int>(i);
            m_hitZones.push_back({hId, trc, static_cast<int>(i)});

            ComPtr<ID2D1RoundedRectangleGeometry> clipGeo;
            if (SUCCEEDED(m_d2dFactory->CreateRoundedRectangleGeometry(
                    D2D1::RoundedRect(trc, 6.0f, 6.0f), clipGeo.GetAddressOf())) && clipGeo && m_themeThumbBitmaps[i]) {
                ComPtr<ID2D1Layer> clipLayer;
                if (SUCCEEDED(m_renderTarget->CreateLayer(nullptr, clipLayer.GetAddressOf())) && clipLayer) {
                    m_renderTarget->PushLayer(D2D1::LayerParameters(trc, clipGeo.Get()), clipLayer.Get());
                    m_renderTarget->DrawBitmap(m_themeThumbBitmaps[i].Get(), trc, 1.0f, D2D1_BITMAP_INTERPOLATION_MODE_LINEAR);
                    m_renderTarget->PopLayer();
                }
            }
            const bool selected = (m_config.backgroundTheme == kOrderedStudioThemes[i]);
            setBrush(selected ? colAccent : colBorder);
            m_renderTarget->DrawRoundedRectangle(
                D2D1::RoundedRect(trc, 6.0f, 6.0f), brush.Get(), selected ? 2.2f : 1.0f);
        }
        curY += thumbH * 2.0f + gapY + 10.0f;
    }

    setBrush(colBorder);
    m_renderTarget->DrawLine(D2D1::Point2F(padL, curY), D2D1::Point2F(padR, curY), brush.Get(), 1.0f);
    curY += 10.0f;

    // (D) 离线语音字幕自动对齐
    drawTextAligned(isZh ? L"语音字幕自动对齐" : L"Auto-Aligned Captions", m_sectionFmt.Get(),
                    D2D1::RectF(padL, curY, padR - 48.0f, curY + 22.0f), colPrimary);
    drawTogglePill(Hit_ToggleCaptions, padR, curY, m_config.autoCaptionsEnabled);
    curY += 23.0f;
    wchar_t capSub[96]{};
    std::swprintf(capSub, std::size(capSub),
                  isZh ? L"已对齐 %zu 条字幕（视频附带 .srt）" : L"%zu synced captions (+ .srt sidecar)",
                  m_timeline.captions.size());
    drawTextAligned(capSub, m_captionFmt.Get(), D2D1::RectF(padL, curY, padR, curY + 18.0f), colSecondary);

    // 底部操作按钮区：打开多格式导出设置 / 直接保留原片
    const bool exporting = m_exporting.load(std::memory_order_acquire);
    const D2D1_RECT_F keepBtnRc = D2D1::RectF(padL, sideCard.bottom - 42.0f, padR, sideCard.bottom - 10.0f);
    const D2D1_RECT_F exportBtnRc = D2D1::RectF(padL, keepBtnRc.top - 42.0f, padR, keepBtnRc.top - 8.0f);

    if (!exporting && !m_exportModalOpen) {
        m_hitZones.push_back({Hit_OpenExportModalBtn, exportBtnRc, -1});
        m_hitZones.push_back({Hit_KeepOriginalBtn, keepBtnRc, -1});
    }

    setBrush(m_hoveredHitId == Hit_OpenExportModalBtn
        ? D2D1::ColorF((std::min)(1.0f, accentRgb.r + 0.06f), (std::min)(1.0f, accentRgb.g + 0.06f), (std::min)(1.0f, accentRgb.b + 0.06f), 1.0f)
        : colAccent);
    m_renderTarget->FillRoundedRectangle(D2D1::RoundedRect(exportBtnRc, 8.0f, 8.0f), brush.Get());
    drawTextAligned(isZh ? L"导出成片 (MP4 / 微信 GIF / WebP)..." : L"Export (MP4 / GIF / WebP)...",
                    m_sectionFmt.Get(), exportBtnRc,
                    D2D1::ColorF(1.0f, 1.0f, 1.0f, 1.0f), DWRITE_TEXT_ALIGNMENT_CENTER);

    setBrush(m_hoveredHitId == Hit_KeepOriginalBtn ? colAccentSoft : colChipBg);
    m_renderTarget->FillRoundedRectangle(D2D1::RoundedRect(keepBtnRc, 8.0f, 8.0f), brush.Get());
    setBrush(colBorder);
    m_renderTarget->DrawRoundedRectangle(D2D1::RoundedRect(keepBtnRc, 8.0f, 8.0f), brush.Get(), 1.0f);
    drawTextAligned(isZh ? L"直接保留原片 (0 秒等待)" : L"Keep Raw MP4 Instantly",
                    m_bodyFmt.Get(), keepBtnRc,
                    exporting ? colSecondary : colPrimary, DWRITE_TEXT_ALIGNMENT_CENTER);

    // 5. 若打开了导出模态窗，叠绘居中导出设置卡片
    if (m_exportModalOpen) {
        renderExportModalOverlay(W, H, dark, isZh, brush.Get());
    }

    if (m_renderTarget->EndDraw() == D2DERR_RECREATE_TARGET) {
        discardDeviceResources();
    }
}

void StudioPreviewWindow::onPlaybackTick() {
    if (!isVisible()) return;

    const DWORD now = GetTickCount();
    const double dt = std::clamp(static_cast<double>(now - m_lastTickMs) / 1000.0, 0.0, 0.15);
    m_lastTickMs = now;

    if (m_exporting.load(std::memory_order_acquire) || m_editingExportName) {
        InvalidateRect(m_hwnd, nullptr, FALSE);
        return;
    }

    if (m_playing && !m_exportModalOpen && !m_scrubbingTimeline && m_durationSec > 0.05) {
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
    m_editingExportName = false;
    m_openDropdown = ExportDropdownKind::None;
    m_cancelExport.store(false, std::memory_order_release);
    m_exportProgress.store(0.01, std::memory_order_release);
    persistStudioConfigToSettings(m_config);
    persistExportPreferencesIfChecked();
    InvalidateRect(m_hwnd, nullptr, FALSE);

    joinExportWorkerIfDone(true);

    const std::string rawPath = m_rawVideoPath;
    const std::string finalPath = buildFinalExportPathUtf8();
    const StudioTimelineData timelineCopy = m_timeline;
    const StudioRenderConfig configCopy = m_config;
    const StudioExportOptions exportOptsCopy = m_exportOpts;
    const double durSec = m_durationSec;

    std::lock_guard lock(m_exportMutex);
    m_exportWorker = std::thread([this, rawPath, finalPath, timelineCopy, configCopy, exportOptsCopy, durSec]() {
        tools3000::core::TraceId::Scope traceScope;
        const bool ok = StudioVideoCompositor::exportStudioMedia(
            rawPath,
            finalPath,
            timelineCopy,
            configCopy,
            exportOptsCopy,
            [this](double p) {
                m_exportProgress.store(p, std::memory_order_relaxed);
            },
            &m_cancelExport);

        const bool canceled = m_cancelExport.load(std::memory_order_acquire);
        m_exporting.store(false, std::memory_order_release);

        tools3000::core::MainThreadDispatcher::instance().post([this, ok, canceled, rawPath, finalPath, durSec]() {
            if (canceled) return;
            const bool isZh = tools3000::core::NativeThemePalette::isUiLanguageChinese();
            if (ok) {
                // 若导出为新路径（如 .gif / .webp 或重命名），自动清理临时原始底片
                const std::filesystem::path rawFs(tools3000::core::WinUtils::utf8ToWstring(rawPath));
                const std::filesystem::path finalFs(tools3000::core::WinUtils::utf8ToWstring(finalPath));
                std::error_code eqEc;
                if (!std::filesystem::equivalent(rawFs, finalFs, eqEc)) {
                    std::error_code rmEc;
                    std::filesystem::remove(rawFs, rmEc);
                }
                LOG_INFO("StudioPreviewWindow: exported studio video successfully: {}", finalPath);
                auto cb = std::move(m_onFinished);
                closeSessionAndCleanup();
                if (cb) cb(finalPath, durSec, true);
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
    m_exportModalOpen = false;
    m_editingExportName = false;
    m_openDropdown = ExportDropdownKind::None;
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

        // ── 导出模态窗热区处理 ─────────────────────────────────────────────────
        if (id >= Hit_ModalDropdownItemBase && id < Hit_ZoomSegmentBase) {
            const int val = it->payload;
            if (m_openDropdown == ExportDropdownKind::Format) {
                applyFormatSmartDefaults(static_cast<StudioExportFormat>(val));
            } else if (m_openDropdown == ExportDropdownKind::Resolution) {
                m_exportOpts.resolution = static_cast<StudioExportResolution>(val);
            } else if (m_openDropdown == ExportDropdownKind::Fps) {
                m_exportOpts.fps = val;
            } else if (m_openDropdown == ExportDropdownKind::Quality) {
                m_exportOpts.quality = static_cast<StudioExportQuality>(val);
            }
            m_openDropdown = ExportDropdownKind::None;
            InvalidateRect(m_hwnd, nullptr, FALSE);
            return;
        }
        if (id == Hit_ModalBackdrop || id == Hit_ModalCloseBtn) {
            if (!m_exporting.load(std::memory_order_acquire)) {
                m_exportModalOpen = false;
                m_editingExportName = false;
                m_openDropdown = ExportDropdownKind::None;
                InvalidateRect(m_hwnd, nullptr, FALSE);
            }
            return;
        }
        if (id == Hit_ModalCardBody) {
            m_editingExportName = false;
            m_openDropdown = ExportDropdownKind::None;
            InvalidateRect(m_hwnd, nullptr, FALSE);
            return;
        }
        if (id == Hit_ModalNameInput) {
            m_editingExportName = true;
            m_openDropdown = ExportDropdownKind::None;
            InvalidateRect(m_hwnd, nullptr, FALSE);
            return;
        }
        if (id == Hit_ModalBrowseDirBtn) {
            m_editingExportName = false;
            m_openDropdown = ExportDropdownKind::None;
            InvalidateRect(m_hwnd, nullptr, FALSE);
            browseExportDirectory();
            return;
        }
        if (id == Hit_ModalFormatSelect || id == Hit_ModalResSelect ||
            id == Hit_ModalFpsSelect || id == Hit_ModalQualitySelect) {
            m_editingExportName = false;
            const ExportDropdownKind targetKind =
                (id == Hit_ModalFormatSelect) ? ExportDropdownKind::Format
                : (id == Hit_ModalResSelect) ? ExportDropdownKind::Resolution
                : (id == Hit_ModalFpsSelect) ? ExportDropdownKind::Fps
                : ExportDropdownKind::Quality;
            m_openDropdown = (m_openDropdown == targetKind) ? ExportDropdownKind::None : targetKind;
            InvalidateRect(m_hwnd, nullptr, FALSE);
            return;
        }
        if (id == Hit_ModalSavePrefsCheck) {
            m_editingExportName = false;
            m_openDropdown = ExportDropdownKind::None;
            m_saveExportPrefs = !m_saveExportPrefs;
            InvalidateRect(m_hwnd, nullptr, FALSE);
            return;
        }
        if (id == Hit_ModalConfirmExportBtn) {
            startAsyncExport();
            return;
        }

        // ── 预览主界面热区处理 ─────────────────────────────────────────────────
        if (id == Hit_CloseBtn || id == Hit_KeepOriginalBtn) {
            if (m_exporting.load(std::memory_order_acquire)) {
                m_cancelExport.store(true, std::memory_order_release);
                joinExportWorkerIfDone(true);
                m_exporting.store(false, std::memory_order_release);
            }
            keepOriginalAndClose();
            return;
        }
        if (id == Hit_OpenExportModalBtn) {
            m_playing = false;
            m_exportModalOpen = true;
            m_editingExportName = false;
            m_openDropdown = ExportDropdownKind::None;
            InvalidateRect(m_hwnd, nullptr, FALSE);
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
        if (id >= Hit_ThemeBase && id < Hit_ThemeBase + 8) {
            m_config.backgroundTheme = kOrderedStudioThemes[static_cast<size_t>(id - Hit_ThemeBase)];
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

    if (!m_exportModalOpen && ly <= 48.0f && m_hwnd) {
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

void StudioPreviewWindow::handleCharInput(wchar_t ch) {
    if (!m_exportModalOpen || !m_editingExportName || m_exporting.load(std::memory_order_acquire)) {
        return;
    }
    if (ch == L'\b') {
        if (!m_exportFileNameStem.empty()) {
            m_exportFileNameStem.pop_back();
            InvalidateRect(m_hwnd, nullptr, FALSE);
        }
        return;
    }
    if (ch == L'\r' || ch == L'\n') {
        m_editingExportName = false;
        InvalidateRect(m_hwnd, nullptr, FALSE);
        return;
    }
    if (isValidFileNameChar(ch) && m_exportFileNameStem.size() < 96) {
        m_exportFileNameStem.push_back(ch);
        InvalidateRect(m_hwnd, nullptr, FALSE);
    }
}

void StudioPreviewWindow::handleKeyDown(WPARAM vk) {
    if (m_exporting.load(std::memory_order_acquire)) return;

    if (m_exportModalOpen) {
        if (vk == VK_ESCAPE) {
            if (m_openDropdown != ExportDropdownKind::None) {
                m_openDropdown = ExportDropdownKind::None;
            } else if (m_editingExportName) {
                m_editingExportName = false;
            } else {
                m_exportModalOpen = false;
            }
            InvalidateRect(m_hwnd, nullptr, FALSE);
            return;
        }
        if (m_editingExportName && (GetAsyncKeyState(VK_CONTROL) & 0x8000) != 0) {
            if (vk == 'V' && OpenClipboard(m_hwnd)) {
                if (HANDLE hData = GetClipboardData(CF_UNICODETEXT)) {
                    if (const auto* wptr = static_cast<const wchar_t*>(GlobalLock(hData))) {
                        for (size_t i = 0; wptr[i] != L'\0' && m_exportFileNameStem.size() < 96; ++i) {
                            if (isValidFileNameChar(wptr[i])) m_exportFileNameStem.push_back(wptr[i]);
                        }
                        GlobalUnlock(hData);
                    }
                }
                CloseClipboard();
                InvalidateRect(m_hwnd, nullptr, FALSE);
                return;
            }
        }
        if (!m_editingExportName && vk == VK_RETURN) {
            startAsyncExport();
            return;
        }
        return;
    }

    if (vk == VK_SPACE) {
        m_playing = !m_playing;
        m_lastTickMs = GetTickCount();
        InvalidateRect(m_hwnd, nullptr, FALSE);
        return;
    }
    if (vk == VK_ESCAPE) {
        keepOriginalAndClose();
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
            const HCURSOR cur = (self->m_hoveredHitId == Hit_ModalNameInput)
                ? LoadCursorW(nullptr, IDC_IBEAM)
                : ((self->m_hoveredHitId != Hit_None &&
                    self->m_hoveredHitId != Hit_ModalBackdrop &&
                    self->m_hoveredHitId != Hit_ModalCardBody)
                    ? LoadCursorW(nullptr, IDC_HAND)
                    : LoadCursorW(nullptr, IDC_ARROW));
            SetCursor(cur);
            return 0;
        }
        case WM_LBUTTONUP:
            self->handleMouseUp();
            return 0;
        case WM_CHAR:
            self->handleCharInput(static_cast<wchar_t>(wParam));
            return 0;
        case WM_KEYDOWN:
            self->handleKeyDown(wParam);
            return 0;
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
