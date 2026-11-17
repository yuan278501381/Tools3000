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
#include <objbase.h>
#include <shlobj.h>
#include <shobjidl.h>
#include <windowsx.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <set>

namespace tools3000::capture {

namespace {

using Microsoft::WRL::ComPtr;

constexpr const wchar_t* kStudioWndClass = L"Tools3000_StudioPreviewWindow";
constexpr UINT_PTR kPlaybackTimerId = 4101;
constexpr UINT kTimerIntervalMs = 16; // 原生 ~60 FPS 超流畅实时预览与尾帧补齐
constexpr UINT WM_STUDIO_CUSTOM_BG_PICKED  = WM_APP + 61;
constexpr UINT WM_STUDIO_EXPORT_DIR_PICKED = WM_APP + 62;

enum HitId : int {
    Hit_None = 0,
    Hit_CloseBtn = 1,
    Hit_PlayPauseBtn = 2,
    Hit_TimelineTopBarScrub = 3,
    Hit_TimelineRulerScrub = 4,
    Hit_StageCanvas = 5,

    // 左侧竖向图标导航栏 (0..3)
    Hit_LeftRailTabBase = 10, // 10..13

    // 时间轴直观拆分切片工具条动作
    Hit_QuickOpenAnnotations = 18,
    Hit_SplitAtPlayhead = 19,
    Hit_DeleteSelectedItem = 20,
    Hit_CycleClipSpeed = 21,
    Hit_UndoBtn = 22,
    Hit_RedoBtn = 23,
    Hit_AddZoomAtPlayhead = 24,
    Hit_ZoomLaneEmpty = 25,

    // 镜头与光标页控制
    Hit_ZoomMode_None = 30,
    Hit_ZoomMode_2D = 31,
    Hit_ZoomMode_3D = 32,
    Hit_ToggleMotionBlur = 33,
    Hit_ZoomPreset_150 = 34,
    Hit_ZoomPreset_185 = 35,
    Hit_ZoomPreset_220 = 36,
    Hit_ToggleSmoothCursor = 37,
    Hit_CursorScale_100 = 38,
    Hit_CursorScale_135 = 39,
    Hit_CursorScale_170 = 40,
    Hit_ToggleCursorHalo = 41,

    // 画布比例与五维精调控制
    Hit_ToggleAutoFrame = 50,
    Hit_AspectBase = 51,       // 51..55 对应 StudioAspectRatio 0..4
    Hit_SliderPadding = 56,
    Hit_SliderInsetBorder = 57,
    Hit_CycleInsetBorderColor = 58,
    Hit_SliderRadius = 59,
    Hit_SliderShadow = 60,
    Hit_SliderBgBlur = 61,

    // 背景分类、翻页、选图与 28 宫格正方形缩略图
    Hit_WallCatBase = 70,      // 70..74 (默认 / 沉稳 / 轻盈 / 风景 / 自定义)
    Hit_WallPickCustom = 75,
    Hit_WallPrevPage = 76,
    Hit_WallNextPage = 77,
    Hit_ThemeSlotBase = 100,   // 100..127 当前页 28 槽正方形缩略图

    // 离线字幕页与顶栏导出按钮
    Hit_ToggleCaptions = 140,
    Hit_OpenExportModalBtn = 145,
    Hit_KeepOriginalBtn = 146,

    // 视频标注抽屉：19 张可视化样式卡片 + 8 色圆点 + 文本编辑框
    Hit_AnnCardBase = 150,     // 150..168 (19 款标注卡片)
    Hit_AnnColorBase = 175,    // 175..182 (8 款高饱和颜色圆点)
    Hit_AnnTextInputBox = 185,

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

    // 三轨时间轴可选中/拖拽片段热区
    Hit_ZoomSegmentBase = 1000, // 1000 + idx
    Hit_VideoClipBase   = 2000, // 2000 + clipIdx
    Hit_AnnSegmentBase  = 3000, // 3000 + idx
};

static constexpr std::array<BeautyBackgroundType, 8> kOrderedStudioThemes = {{
    BeautyBackgroundType::MidnightGraphite, // [沉稳] 深海曜石
    BeautyBackgroundType::StudioSlate,      // [沉稳] 暮夜星云
    BeautyBackgroundType::SilkMist,         // [沉稳] 丝绸青雾
    BeautyBackgroundType::PureMinimal,      // [轻盈] 极简云白
    BeautyBackgroundType::PaperChalk,       // [轻盈] 暖杏羊绒
    BeautyBackgroundType::AiryGlacier,      // [轻盈] 冰川薄荷
    BeautyBackgroundType::DuskMountains,    // [风景] 暮色群峰
    BeautyBackgroundType::MistyLakeDawn,    // [风景] 拂晓晨湖
}};

static constexpr std::array<const char*, 8> kAnnotationPaletteHex = {{
    "#EC4899", // 醒目玫粉
    "#EF4444", // 珊瑚红
    "#F59E0B", // 琥珀金
    "#10B981", // 翡翠绿
    "#3B82F6", // 晴空蓝
    "#8B5CF6", // 电光紫
    "#FFFFFF", // 纯白
    "#0F172A", // 深曜黑
}};

static constexpr std::array<const char*, 6> kInsetBorderColors = {{
    "#FFFFFF", "#F8FAFC", "#F59E0B", "#38BDF8", "#EC4899", "#0F172A"
}};

int64_t queryQpcNow() noexcept {
    LARGE_INTEGER li{};
    QueryPerformanceCounter(&li);
    return li.QuadPart;
}

int64_t queryQpcFreq() noexcept {
    static const int64_t f = []() {
        LARGE_INTEGER li{};
        return (QueryPerformanceFrequency(&li) && li.QuadPart > 0) ? li.QuadPart : 10'000'000LL;
    }();
    return f;
}

D2D1_COLOR_F hexToD2DColor(const std::string& hex, float alpha = 1.0f) noexcept {
    if (hex.size() == 7 && hex[0] == '#') {
        auto h2i = [](char c) -> int {
            if (c >= '0' && c <= '9') return c - '0';
            if (c >= 'a' && c <= 'f') return c - 'a' + 10;
            if (c >= 'A' && c <= 'F') return c - 'A' + 10;
            return 0;
        };
        const int r = (h2i(hex[1]) << 4) | h2i(hex[2]);
        const int g = (h2i(hex[3]) << 4) | h2i(hex[4]);
        const int b = (h2i(hex[5]) << 4) | h2i(hex[6]);
        return D2D1::ColorF(r / 255.0f, g / 255.0f, b / 255.0f, alpha);
    }
    return D2D1::ColorF(0.93f, 0.28f, 0.60f, alpha);
}

const wchar_t* themeDisplayName(BeautyBackgroundType t, bool isZh) noexcept {
    switch (t) {
        case BeautyBackgroundType::MidnightGraphite: return isZh ? L"深海曜石" : L"Nordic Obsidian";
        case BeautyBackgroundType::StudioSlate:      return isZh ? L"暮夜星云" : L"Velvet Dusk";
        case BeautyBackgroundType::SilkMist:         return isZh ? L"丝绸青雾" : L"Silk Mist";
        case BeautyBackgroundType::PureMinimal:      return isZh ? L"极简云白" : L"Pure Minimal";
        case BeautyBackgroundType::PaperChalk:       return isZh ? L"暖杏羊绒" : L"Warm Cashmere";
        case BeautyBackgroundType::AiryGlacier:      return isZh ? L"冰川薄荷" : L"Airy Glacier";
        case BeautyBackgroundType::DuskMountains:    return isZh ? L"暮色群峰" : L"Dusk Peaks";
        case BeautyBackgroundType::MistyLakeDawn:    return isZh ? L"拂晓晨湖" : L"Misty Lake";
        default:                                     return isZh ? L"深海曜石" : L"Nordic Obsidian";
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
        case StudioExportFormat::MP4_H264:      return isZh ? L"MP4 (通用视频 · 可放大)" : L"MP4 (H.264 Video)";
        case StudioExportFormat::MP4_HEVC:      return isZh ? L"MP4 (HEVC 超清 · 可放大)" : L"MP4 (HEVC Video)";
        case StudioExportFormat::GIF_WeChat:    return isZh ? L"GIF (微信表情 · 免点直播)" : L"GIF (WeChat Sticker)";
        case StudioExportFormat::GIF_HD:        return isZh ? L"GIF (高清大图 · 可双击放大)" : L"GIF (HD · Zoomable)";
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
    cm.set<int>("/recording/studioZoomMode",
                !cfg.autoZoomEnabled ? 0 : (cfg.zoomMode == StudioZoomMode::Motion3D ? 2 : 1));
    cm.set<double>("/recording/studioZoomFactor", static_cast<double>(cfg.defaultZoomFactor));
    cm.set<bool>("/recording/studioSmoothCursor", cfg.smoothCursorEnabled);
    cm.set<double>("/recording/studioCursorScale", static_cast<double>(cfg.cursorScale));
    cm.set<bool>("/recording/studioAutoFrame", cfg.autoFrameEnabled);
    cm.set<double>("/recording/studioPaddingRatio", static_cast<double>(cfg.paddingRatio));
    cm.set<double>("/recording/studioInsetBorder", static_cast<double>(cfg.insetBorderWidth));
    cm.set<std::string>("/recording/studioInsetBorderColor", cfg.insetBorderColorHex);
    cm.set<double>("/recording/studioCornerRadius", static_cast<double>(cfg.cornerRadius));
    cm.set<double>("/recording/studioBgBlur", static_cast<double>(cfg.backgroundBlur));
    cm.set<double>("/recording/studioShadow", static_cast<double>(cfg.shadowIntensity));
    cm.set<bool>("/recording/studioMotionBlur", cfg.motionBlurEnabled);
    cm.set<bool>("/recording/studioCursorHalo", cfg.cursorHighlightHalo);
    cm.set<std::string>("/recording/studioCustomBg", cfg.customBackgroundPath);
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

void drawScissorsIcon(ID2D1RenderTarget* rt, ID2D1Brush* brush, float cx, float cy) {
    rt->DrawEllipse(D2D1::Ellipse({cx - 3.6f, cy - 3.2f}, 2.4f, 2.4f), brush, 1.4f);
    rt->DrawEllipse(D2D1::Ellipse({cx - 3.6f, cy + 3.2f}, 2.4f, 2.4f), brush, 1.4f);
    rt->DrawLine({cx - 1.5f, cy - 1.8f}, {cx + 5.5f, cy + 3.8f}, brush, 1.5f);
    rt->DrawLine({cx - 1.5f, cy + 1.8f}, {cx + 5.5f, cy - 3.8f}, brush, 1.5f);
}

std::wstring queryKnownFolderPath(REFKNOWNFOLDERID rfid) {
    PWSTR raw = nullptr;
    std::wstring res;
    if (SUCCEEDED(SHGetKnownFolderPath(rfid, 0, nullptr, &raw)) && raw) {
        res = raw;
        CoTaskMemFree(raw);
    }
    return res;
}

bool isTextLikeAnnotation(StudioAnnotationKind k) noexcept {
    return k == StudioAnnotationKind::Text ||
           k == StudioAnnotationKind::TextPlain ||
           k == StudioAnnotationKind::TextBubble ||
           k == StudioAnnotationKind::TextDashed ||
           k == StudioAnnotationKind::TextNeon ||
           k == StudioAnnotationKind::StepBadge;
}

bool isPointDirectedAnnotation(StudioAnnotationKind k) noexcept {
    return k == StudioAnnotationKind::Arrow ||
           k == StudioAnnotationKind::BoldArrow ||
           k == StudioAnnotationKind::GlowLine ||
           k == StudioAnnotationKind::DashedLine;
}

} // namespace

StudioPreviewWindow& StudioPreviewWindow::instance() {
    static StudioPreviewWindow inst;
    return inst;
}

bool StudioPreviewWindow::isVisible() const noexcept {
    return m_hwnd && IsWindow(m_hwnd) && IsWindowVisible(m_hwnd);
}

// ── 磁性波纹拆分剪辑 (Split -> Select -> Delete / Speed) + Undo/Redo ──────────

void StudioPreviewWindow::pushUndoSnapshot() {
    if (m_undoStack.size() >= 40) {
        m_undoStack.erase(m_undoStack.begin());
    }
    m_undoStack.push_back({m_videoClips, m_timeline.zoomSegments, m_timeline.annotations, m_playheadSec});
    m_redoStack.clear();
}

void StudioPreviewWindow::syncCutSegmentsFromVideoClips() {
    const int64_t rawTotalMs = (std::max<int64_t>)(100, static_cast<int64_t>(std::llround(m_durationSec * 1000.0)));
    m_timeline.cutSegments = StudioTimelineAnalyzer::buildCutSegmentsFromClips(m_videoClips, rawTotalMs);
    const double keptSec = computeEffectiveDurationSec();
    m_playheadSec = std::clamp(m_playheadSec, 0.0, keptSec);
    m_hoverSkimSec = std::clamp(m_hoverSkimSec, 0.0, keptSec);
    if (m_selectedClipIdx >= static_cast<int>(m_videoClips.size())) {
        m_selectedClipIdx = static_cast<int>(m_videoClips.size()) - 1;
    }
}

void StudioPreviewWindow::performUndo() {
    if (m_undoStack.empty()) return;
    m_redoStack.push_back({m_videoClips, m_timeline.zoomSegments, m_timeline.annotations, m_playheadSec});
    auto snap = std::move(m_undoStack.back());
    m_undoStack.pop_back();
    m_videoClips = std::move(snap.videoClips);
    m_timeline.zoomSegments = std::move(snap.zoomSegments);
    m_timeline.annotations = std::move(snap.annotations);
    m_playheadSec = snap.playheadKeptSec;
    m_selectedClipIdx = -1;
    m_selectedZoomIdx = -1;
    m_selectedAnnIdx = -1;
    syncCutSegmentsFromVideoClips();
    rebakeTimelinePreview();
    requestImmediatePreviewRefresh(true);
}

void StudioPreviewWindow::performRedo() {
    if (m_redoStack.empty()) return;
    m_undoStack.push_back({m_videoClips, m_timeline.zoomSegments, m_timeline.annotations, m_playheadSec});
    auto snap = std::move(m_redoStack.back());
    m_redoStack.pop_back();
    m_videoClips = std::move(snap.videoClips);
    m_timeline.zoomSegments = std::move(snap.zoomSegments);
    m_timeline.annotations = std::move(snap.annotations);
    m_playheadSec = snap.playheadKeptSec;
    m_selectedClipIdx = -1;
    m_selectedZoomIdx = -1;
    m_selectedAnnIdx = -1;
    syncCutSegmentsFromVideoClips();
    rebakeTimelinePreview();
    requestImmediatePreviewRefresh(true);
}

double StudioPreviewWindow::currentDisplayedSourceSec() const noexcept {
    const double activeKeptSec = m_hoverSkimming ? m_hoverSkimSec : m_playheadSec;
    const int64_t keptMs = static_cast<int64_t>(std::llround(std::max(0.0, activeKeptSec) * 1000.0));
    const int64_t srcMs = StudioTimelineAnalyzer::mapKeptMsToSourceMs(m_videoClips, keptMs, nullptr);
    return std::clamp(static_cast<double>(srcMs) / 1000.0, 0.0, m_durationSec);
}

void StudioPreviewWindow::splitClipAtCurrentCursor(bool preferSkimmer) {
    m_playing = false;
    if (preferSkimmer && m_hoverSkimming) {
        m_playheadSec = m_hoverSkimSec;
    }
    const int64_t keptMs = static_cast<int64_t>(std::llround(m_playheadSec * 1000.0));
    int clipIdx = -1;
    const int64_t srcMs = StudioTimelineAnalyzer::mapKeptMsToSourceMs(m_videoClips, keptMs, &clipIdx);
    if (clipIdx < 0 || clipIdx >= static_cast<int>(m_videoClips.size())) return;

    const auto& targetClip = m_videoClips[static_cast<size_t>(clipIdx)];
    if (srcMs < targetClip.srcStartMs + 140 || srcMs > targetClip.srcEndMs - 140) {
        // 指针紧贴切片边缘时无需重复切出极碎微片
        return;
    }

    pushUndoSnapshot();
    const int rightIdx = StudioTimelineAnalyzer::splitClipAtSourceMs(m_videoClips, srcMs, 140);
    if (rightIdx >= 0) {
        m_selectedClipIdx = rightIdx;
        m_selectedZoomIdx = -1;
        m_selectedAnnIdx = -1;
        syncCutSegmentsFromVideoClips();
        requestImmediatePreviewRefresh(true);
    } else {
        m_undoStack.pop_back();
    }
}

void StudioPreviewWindow::deleteSelectedTimelineEntity() {
    if (m_selectedAnnIdx >= 0 && m_selectedAnnIdx < static_cast<int>(m_timeline.annotations.size())) {
        pushUndoSnapshot();
        m_timeline.annotations.erase(m_timeline.annotations.begin() + m_selectedAnnIdx);
        m_selectedAnnIdx = -1;
        m_editingAnnotationText = false;
        requestImmediatePreviewRefresh(true);
        return;
    }
    if (m_selectedZoomIdx >= 0 && m_selectedZoomIdx < static_cast<int>(m_timeline.zoomSegments.size())) {
        pushUndoSnapshot();
        m_timeline.zoomSegments.erase(m_timeline.zoomSegments.begin() + m_selectedZoomIdx);
        m_selectedZoomIdx = -1;
        rebakeTimelinePreview();
        requestImmediatePreviewRefresh(true);
        return;
    }
    if (m_selectedClipIdx >= 0 && m_selectedClipIdx < static_cast<int>(m_videoClips.size()) &&
        m_videoClips.size() > 1) {
        pushUndoSnapshot();
        int64_t keptBefore = 0;
        for (int i = 0; i < m_selectedClipIdx; ++i) {
            keptBefore += m_videoClips[static_cast<size_t>(i)].keptDurationMs();
        }
        m_videoClips.erase(m_videoClips.begin() + m_selectedClipIdx);
        m_selectedClipIdx = (std::min)(m_selectedClipIdx, static_cast<int>(m_videoClips.size()) - 1);
        syncCutSegmentsFromVideoClips();
        m_playheadSec = std::clamp(static_cast<double>(keptBefore) / 1000.0, 0.0, computeEffectiveDurationSec());
        m_hoverSkimming = false;
        requestImmediatePreviewRefresh(true);
    }
}

void StudioPreviewWindow::cycleSelectedClipSpeed() {
    int idx = m_selectedClipIdx;
    if (idx < 0 || idx >= static_cast<int>(m_videoClips.size())) {
        const int64_t keptMs = static_cast<int64_t>(std::llround(m_playheadSec * 1000.0));
        StudioTimelineAnalyzer::mapKeptMsToSourceMs(m_videoClips, keptMs, &idx);
    }
    if (idx < 0 || idx >= static_cast<int>(m_videoClips.size())) return;

    pushUndoSnapshot();
    auto& clip = m_videoClips[static_cast<size_t>(idx)];
    if (clip.speed < 1.25f) clip.speed = 1.5f;
    else if (clip.speed < 1.75f) clip.speed = 2.0f;
    else clip.speed = 1.0f;
    m_selectedClipIdx = idx;
    syncCutSegmentsFromVideoClips();
    requestImmediatePreviewRefresh(true);
}

void StudioPreviewWindow::requestImmediatePreviewRefresh(bool forceNow) {
    if (!m_hwnd || !IsWindow(m_hwnd)) return;

    const int64_t nowQpc = queryQpcNow();
    const double elapsedMs = static_cast<double>(nowQpc - m_lastComposeQpcTick) * 1000.0 /
                             static_cast<double>(queryQpcFreq());

    // 120 FPS (8.0ms) 节流合并：鼠标 1000Hz 高频滑动时既立即同步上屏零拖影，又不积压消息队列
    if (forceNow || elapsedMs >= 8.0) {
        m_lastComposeQpcTick = nowQpc;
        m_pendingSkimCompose = false;
        updateCurrentComposedBitmap();
        RedrawWindow(m_hwnd, nullptr, nullptr, RDW_INVALIDATE | RDW_UPDATENOW);
    } else {
        m_pendingSkimCompose = true;
        InvalidateRect(m_hwnd, nullptr, FALSE);
    }
}

void StudioPreviewWindow::discoverLocalWallpapers() {
    m_allWallpapers.clear();
    const bool isZh = tools3000::core::NativeThemePalette::isUiLanguageChinese();

    for (size_t i = 0; i < kOrderedStudioThemes.size(); ++i) {
        WallpaperEntry entry{};
        entry.isBuiltin = true;
        entry.builtinTheme = kOrderedStudioThemes[i];
        entry.displayName = themeDisplayName(entry.builtinTheme, isZh);
        entry.category = 0;
        m_allWallpapers.push_back(std::move(entry));
    }

    std::vector<std::filesystem::path> scanDirs;
    const std::wstring localAppData = queryKnownFolderPath(FOLDERID_LocalAppData);
    const std::wstring roamingAppData = queryKnownFolderPath(FOLDERID_RoamingAppData);
    if (!localAppData.empty()) {
        scanDirs.push_back(std::filesystem::path(localAppData) / L"Tools3000" / L"wallpapers");
    }
    if (!roamingAppData.empty()) {
        scanDirs.push_back(std::filesystem::path(roamingAppData) / L"iMobie" / L"FocuSee" / L"BackgroundData" / L"source");
    }

    std::set<std::wstring> seenNames;
    std::vector<std::filesystem::path> foundFiles;
    std::error_code ec;
    for (const auto& dir : scanDirs) {
        if (!std::filesystem::is_directory(dir, ec)) continue;
        for (const auto& de : std::filesystem::directory_iterator(dir, ec)) {
            if (!de.is_regular_file(ec)) continue;
            const std::wstring ext = tools3000::core::WinUtils::toLower(de.path().extension().wstring());
            if (ext != L".jpg" && ext != L".jpeg" && ext != L".png" && ext != L".webp") continue;
            const std::wstring fnameLower = tools3000::core::WinUtils::toLower(de.path().filename().wstring());
            if (seenNames.insert(fnameLower).second) {
                foundFiles.push_back(de.path());
            }
        }
    }

    std::sort(foundFiles.begin(), foundFiles.end(), [](const auto& a, const auto& b) {
        return a.filename().wstring() < b.filename().wstring();
    });

    for (const auto& fp : foundFiles) {
        const std::wstring stem = fp.stem().wstring();
        const std::wstring lower = tools3000::core::WinUtils::toLower(stem);
        int cat = 1;
        if (lower.find(L"daytime") != std::wstring::npos ||
            lower.find(L"light") != std::wstring::npos ||
            lower.find(L"paper") != std::wstring::npos) {
            cat = 2;
        } else if (lower.find(L"landscape") != std::wstring::npos ||
                   lower.find(L"mountain") != std::wstring::npos ||
                   lower.find(L"lake") != std::wstring::npos ||
                   lower.find(L"nature") != std::wstring::npos) {
            cat = 3;
        } else {
            cat = 1;
        }

        WallpaperEntry entry{};
        entry.isBuiltin = false;
        entry.filePathUtf8 = tools3000::core::WinUtils::wstringToUtf8(fp.wstring());
        entry.displayName = stem;
        entry.category = cat;
        m_allWallpapers.push_back(std::move(entry));
    }

    if (!m_config.customBackgroundPath.empty()) {
        bool alreadyListed = false;
        for (const auto& w : m_allWallpapers) {
            if (!w.isBuiltin && w.filePathUtf8 == m_config.customBackgroundPath) {
                alreadyListed = true;
                break;
            }
        }
        if (!alreadyListed) {
            const std::filesystem::path cp(tools3000::core::WinUtils::utf8ToWstring(m_config.customBackgroundPath));
            if (std::filesystem::exists(cp, ec)) {
                WallpaperEntry customEntry{};
                customEntry.isBuiltin = false;
                customEntry.filePathUtf8 = m_config.customBackgroundPath;
                customEntry.displayName = cp.stem().wstring();
                customEntry.category = 4;
                m_allWallpapers.push_back(std::move(customEntry));
            }
        }
    }

    m_wallpapersDiscovered = true;
    rebuildFilteredWallpapers();
}

void StudioPreviewWindow::rebuildFilteredWallpapers() {
    m_filteredWallpaperIndices.clear();
    if (m_wallpaperCategory == 0) {
        for (int i = 0; i < static_cast<int>(m_allWallpapers.size()); ++i) {
            if (m_allWallpapers[static_cast<size_t>(i)].isBuiltin) {
                m_filteredWallpaperIndices.push_back(i);
            }
        }
        for (int i = 0; i < static_cast<int>(m_allWallpapers.size()); ++i) {
            if (!m_allWallpapers[static_cast<size_t>(i)].isBuiltin) {
                m_filteredWallpaperIndices.push_back(i);
            }
        }
    } else {
        for (int i = 0; i < static_cast<int>(m_allWallpapers.size()); ++i) {
            const auto& w = m_allWallpapers[static_cast<size_t>(i)];
            if (w.category == m_wallpaperCategory) {
                m_filteredWallpaperIndices.push_back(i);
            } else if (w.isBuiltin) {
                if (m_wallpaperCategory == 1 &&
                    (w.builtinTheme == BeautyBackgroundType::MidnightGraphite ||
                     w.builtinTheme == BeautyBackgroundType::StudioSlate ||
                     w.builtinTheme == BeautyBackgroundType::SilkMist)) {
                    m_filteredWallpaperIndices.push_back(i);
                } else if (m_wallpaperCategory == 2 &&
                           (w.builtinTheme == BeautyBackgroundType::PureMinimal ||
                            w.builtinTheme == BeautyBackgroundType::PaperChalk ||
                            w.builtinTheme == BeautyBackgroundType::AiryGlacier)) {
                    m_filteredWallpaperIndices.push_back(i);
                } else if (m_wallpaperCategory == 3 &&
                           (w.builtinTheme == BeautyBackgroundType::DuskMountains ||
                            w.builtinTheme == BeautyBackgroundType::MistyLakeDawn)) {
                    m_filteredWallpaperIndices.push_back(i);
                }
            }
        }
    }

    const int perPage = static_cast<int>(kWallpaperSlotsPerPage);
    const int totalPages = (std::max)(1, static_cast<int>((static_cast<int>(m_filteredWallpaperIndices.size()) + perPage - 1) / perPage));
    m_wallpaperPage = std::clamp(m_wallpaperPage, 0, totalPages - 1);
    invalidateWallpaperPageBitmaps();
}

void StudioPreviewWindow::invalidateWallpaperPageBitmaps() {
    for (auto& bmp : m_themeThumbBitmaps) {
        bmp.Reset();
    }
    m_wallpaperThumbsDirty = true;
}

void StudioPreviewWindow::joinFileDialogWorkerIfDone(bool wait) {
    std::thread toJoin;
    {
        std::lock_guard lock(m_fileDialogMutex);
        if (m_fileDialogWorker.joinable() && (wait || !m_fileDialogInFlight.load(std::memory_order_acquire))) {
            if (std::this_thread::get_id() != m_fileDialogWorker.get_id()) {
                toJoin = std::move(m_fileDialogWorker);
            }
        }
    }
    if (toJoin.joinable()) {
        toJoin.join();
    }
}

void StudioPreviewWindow::browseCustomWallpaperImageAsync() {
    if (m_fileDialogInFlight.exchange(true, std::memory_order_acq_rel)) {
        return;
    }
    joinFileDialogWorkerIfDone(true);

    const HWND targetHwnd = m_hwnd;
    const bool isZh = tools3000::core::NativeThemePalette::isUiLanguageChinese();

    std::lock_guard lock(m_fileDialogMutex);
    m_fileDialogWorker = std::thread([this, targetHwnd, isZh]() {
        const HRESULT comHr = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
        const bool needUninit = SUCCEEDED(comHr);
        struct ScopedCom {
            bool active;
            ~ScopedCom() noexcept { if (active) CoUninitialize(); }
        } comScope{needUninit};

        std::wstring chosenPath;
        if (SUCCEEDED(comHr) || comHr == RPC_E_CHANGED_MODE) {
            ComPtr<IFileOpenDialog> dlg;
            if (SUCCEEDED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(dlg.GetAddressOf()))) && dlg) {
                const COMDLG_FILTERSPEC spec[] = {
                    {isZh ? L"图片文件 (*.jpg;*.jpeg;*.png;*.webp;*.bmp)" : L"Image Files (*.jpg;*.jpeg;*.png;*.webp;*.bmp)",
                     L"*.jpg;*.jpeg;*.png;*.webp;*.bmp"},
                    {isZh ? L"所有文件 (*.*)" : L"All Files (*.*)", L"*.*"}
                };
                dlg->SetFileTypes(static_cast<UINT>(std::size(spec)), spec);
                dlg->SetTitle(isZh ? L"选择自定义背景图片" : L"Select Custom Background Image");
                DWORD fos = 0;
                if (SUCCEEDED(dlg->GetOptions(&fos))) {
                    dlg->SetOptions(fos | FOS_FORCEFILESYSTEM | FOS_FILEMUSTEXIST | FOS_NOCHANGEDIR | FOS_DONTADDTORECENT);
                }
                if (SUCCEEDED(dlg->Show(nullptr))) {
                    ComPtr<IShellItem> resItem;
                    if (SUCCEEDED(dlg->GetResult(resItem.GetAddressOf())) && resItem) {
                        PWSTR rawPath = nullptr;
                        if (SUCCEEDED(resItem->GetDisplayName(SIGDN_FILESYSPATH, &rawPath)) && rawPath) {
                            chosenPath = rawPath;
                            CoTaskMemFree(rawPath);
                        }
                    }
                }
            }
        }

        if (!chosenPath.empty()) {
            std::lock_guard dLock(m_fileDialogMutex);
            m_pendingCustomBgWide = std::move(chosenPath);
        }
        m_fileDialogInFlight.store(false, std::memory_order_release);
        if (targetHwnd && IsWindow(targetHwnd)) {
            PostMessageW(targetHwnd, WM_STUDIO_CUSTOM_BG_PICKED, 0, 0);
        }
    });
}

void StudioPreviewWindow::browseExportDirectoryAsync() {
    if (m_fileDialogInFlight.exchange(true, std::memory_order_acq_rel)) {
        return;
    }
    joinFileDialogWorkerIfDone(true);

    const HWND targetHwnd = m_hwnd;
    const std::wstring initDir = m_exportDirectoryWide;
    const bool isZh = tools3000::core::NativeThemePalette::isUiLanguageChinese();

    std::lock_guard lock(m_fileDialogMutex);
    m_fileDialogWorker = std::thread([this, targetHwnd, initDir, isZh]() {
        const HRESULT comHr = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
        const bool needUninit = SUCCEEDED(comHr);
        struct ScopedCom {
            bool active;
            ~ScopedCom() noexcept { if (active) CoUninitialize(); }
        } comScope{needUninit};

        std::wstring chosenDir;
        if (SUCCEEDED(comHr) || comHr == RPC_E_CHANGED_MODE) {
            ComPtr<IFileOpenDialog> dlg;
            if (SUCCEEDED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(dlg.GetAddressOf()))) && dlg) {
                DWORD fos = 0;
                if (SUCCEEDED(dlg->GetOptions(&fos))) {
                    dlg->SetOptions(fos | FOS_PICKFOLDERS | FOS_FORCEFILESYSTEM | FOS_NOCHANGEDIR | FOS_DONTADDTORECENT);
                }
                dlg->SetTitle(isZh ? L"选择导出保存目录" : L"Select Export Directory");
                if (!initDir.empty()) {
                    ComPtr<IShellItem> initFolder;
                    if (SUCCEEDED(SHCreateItemFromParsingName(initDir.c_str(), nullptr, IID_PPV_ARGS(initFolder.GetAddressOf()))) && initFolder) {
                        dlg->SetFolder(initFolder.Get());
                    }
                }
                if (SUCCEEDED(dlg->Show(nullptr))) {
                    ComPtr<IShellItem> resItem;
                    if (SUCCEEDED(dlg->GetResult(resItem.GetAddressOf())) && resItem) {
                        PWSTR rawPath = nullptr;
                        if (SUCCEEDED(resItem->GetDisplayName(SIGDN_FILESYSPATH, &rawPath)) && rawPath) {
                            chosenDir = rawPath;
                            CoTaskMemFree(rawPath);
                        }
                    }
                }
            }
        }

        if (!chosenDir.empty()) {
            std::lock_guard dLock(m_fileDialogMutex);
            m_pendingExportDirWide = std::move(chosenDir);
        }
        m_fileDialogInFlight.store(false, std::memory_order_release);
        if (targetHwnd && IsWindow(targetHwnd)) {
            PostMessageW(targetHwnd, WM_STUDIO_EXPORT_DIR_PICKED, 0, 0);
        }
    });
}

void StudioPreviewWindow::onCustomWallpaperPicked() {
    std::wstring picked;
    {
        std::lock_guard lock(m_fileDialogMutex);
        picked = std::move(m_pendingCustomBgWide);
        m_pendingCustomBgWide.clear();
    }
    joinFileDialogWorkerIfDone(false);
    if (picked.empty()) return;

    const std::string utf8 = tools3000::core::WinUtils::wstringToUtf8(picked);
    m_config.customBackgroundPath = utf8;
    m_config.autoFrameEnabled = true;

    bool exists = false;
    for (const auto& w : m_allWallpapers) {
        if (!w.isBuiltin && w.filePathUtf8 == utf8) {
            exists = true;
            break;
        }
    }
    if (!exists) {
        WallpaperEntry customEntry{};
        customEntry.isBuiltin = false;
        customEntry.filePathUtf8 = utf8;
        customEntry.displayName = std::filesystem::path(picked).stem().wstring();
        customEntry.category = 4;
        m_allWallpapers.push_back(std::move(customEntry));
    }
    m_wallpaperCategory = 4;
    m_wallpaperPage = 0;
    rebuildFilteredWallpapers();
    updateCurrentComposedBitmap();
    if (m_hwnd) {
        SetForegroundWindow(m_hwnd);
        InvalidateRect(m_hwnd, nullptr, FALSE);
    }
}

void StudioPreviewWindow::onExportDirectoryPicked() {
    std::wstring picked;
    {
        std::lock_guard lock(m_fileDialogMutex);
        picked = std::move(m_pendingExportDirWide);
        m_pendingExportDirWide.clear();
    }
    joinFileDialogWorkerIfDone(false);
    if (!picked.empty()) {
        m_exportDirectoryWide = std::move(picked);
    }
    if (m_hwnd) {
        SetForegroundWindow(m_hwnd);
        InvalidateRect(m_hwnd, nullptr, FALSE);
    }
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
        desiredPhys, work, tools3000::core::dpi::scaleMetric(20, m_dpiScale));
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

double StudioPreviewWindow::computeEffectiveDurationSec() const {
    if (m_videoClips.empty()) return m_durationSec;
    const int64_t keptMs = StudioTimelineAnalyzer::computeClipsKeptDurationMs(m_videoClips);
    return (std::max)(0.2, static_cast<double>(keptMs) / 1000.0);
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
        // 默认 1080P (长边 >= 1080px > 微信 1000px 表情阈值)，发到微信按普通高清图片展示，支持双击打开大图查看器放大
        m_exportOpts.resolution = StudioExportResolution::Res1080P;
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

bool StudioPreviewWindow::showSession(
    const std::string& rawVideoPathUtf8,
    StudioTimelineData timeline,
    const StudioRenderConfig& initialConfig,
    int bitrateMbps,
    CompletionHandler onFinished,
    std::vector<cv::Mat> preloadedDeck) {
    if (rawVideoPathUtf8.empty()) return false;

    joinExportWorkerIfDone(true);
    joinFileDialogWorkerIfDone(true);

    std::vector<cv::Mat> deck = std::move(preloadedDeck);
    int origW = timeline.sourceWidth;
    int origH = timeline.sourceHeight;
    double durSec = (timeline.durationMs > 0) ? (timeline.durationMs / 1000.0) : 0.0;

    if (deck.empty()) {
        if (!StudioVideoCompositor::extractPreviewDeck(rawVideoPathUtf8, 768, 480, deck, origW, origH, durSec) ||
            deck.empty()) {
            LOG_WARN("StudioPreviewWindow: failed to extract preview deck from {}, falling back to direct raw completion",
                     rawVideoPathUtf8);
            if (onFinished) {
                onFinished(rawVideoPathUtf8, timeline.durationMs / 1000.0, false);
            }
            return false;
        }
    } else {
        if (origW <= 0) origW = deck.front().cols;
        if (origH <= 0) origH = deck.front().rows;
        if (durSec <= 0.05) {
            durSec = (std::max)(0.2, static_cast<double>(deck.size()) / 30.0);
        }
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
    if (!m_config.autoZoomEnabled) {
        m_config.zoomMode = StudioZoomMode::None;
    } else if (m_config.zoomMode == StudioZoomMode::None) {
        m_config.zoomMode = StudioZoomMode::Zoom2D;
    }
    if (m_timeline.zoomSegments.empty()) {
        m_timeline.zoomSegments = StudioTimelineAnalyzer::buildAutoZoomSegments(
            m_timeline, m_config.defaultZoomFactor, m_config.typingAutoZoom);
    }

    m_bitrateMbps = std::clamp(bitrateMbps, 2, 80);
    m_durationSec = (durSec > 0.05) ? durSec : (std::max)(0.1, m_timeline.durationMs / 1000.0);
    m_onFinished = std::move(onFinished);
    m_previewDeckBgr = std::move(deck);
    m_origWidth = origW;
    m_origHeight = origH;

    const int64_t rawTotalMs = (std::max<int64_t>)(100, static_cast<int64_t>(std::llround(m_durationSec * 1000.0)));
    if (m_timeline.audioWaveformPeaks.empty()) {
        StudioVideoCompositor::extractAudioWaveformPeaks(m_rawVideoPath, 2048, m_timeline.audioWaveformPeaks);
    }
    m_waveformReferencePeak = 0.08f;
    m_hasActiveAudioWaveform = false;
    {
        std::vector<float> activePeaks;
        activePeaks.reserve(m_timeline.audioWaveformPeaks.size());
        for (float pk : m_timeline.audioWaveformPeaks) {
            if (pk >= 0.008f) {
                m_hasActiveAudioWaveform = true;
                activePeaks.push_back(pk);
            }
        }
        if (!activePeaks.empty()) {
            // 采用 P96 (96% 稳健分位数) 作为声学归一化基准：防止单次敲击桌面/爆麦瞬态尖峰将整段正常人声波形压扁成细线
            std::sort(activePeaks.begin(), activePeaks.end());
            const size_t p96Idx = std::min<size_t>(
                activePeaks.size() - 1,
                static_cast<size_t>(std::floor(static_cast<double>(activePeaks.size() - 1) * 0.96)));
            m_waveformReferencePeak = (std::max)(0.06f, activePeaks[p96Idx]);
        }
    }

    m_videoClips = StudioTimelineAnalyzer::buildClipsFromCutSegments(rawTotalMs, m_timeline.cutSegments);
    m_undoStack.clear();
    m_redoStack.clear();

    m_activeTab = LeftRailTab::CanvasBg;
    m_nextStepBadgeNumber = 1;
    m_editingAnnotationText = false;
    m_stageAnnDrag = StageAnnDragKind::None;
    m_selectedClipIdx = -1;
    m_selectedZoomIdx = -1;
    m_selectedAnnIdx = -1;
    m_trackDragKind = TrackDragKind::None;
    m_activeSlider = SliderDragKind::None;

    discoverLocalWallpapers();
    initExportModalDefaults();

    m_exporting.store(false, std::memory_order_release);
    m_cancelExport.store(false, std::memory_order_release);
    m_exportProgress.store(0.0, std::memory_order_release);

    m_playheadSec = 0.0;
    m_hoverSkimming = false;
    m_hoverSkimSec = 0.0;
    m_pendingSkimCompose = false;
    m_playing = true;
    m_scrubbingTimeline = false;
    m_lastTickMs = GetTickCount();
    m_lastComposeQpcTick = queryQpcNow();

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
        desiredPhys, work, tools3000::core::dpi::scaleMetric(20, m_dpiScale));
    const int posX = work.left + ((work.right - work.left) - fitted.cx) / 2;
    const int posY = work.top + ((work.bottom - work.top) - fitted.cy) / 2;

    if (IsIconic(m_hwnd)) {
        ShowWindow(m_hwnd, SW_RESTORE);
    }

    // 录屏停止通常由全局热键或 WS_EX_NOACTIVATE 悬浮条触发，此时当前前台进程是外部被录窗口。
    // 先借 HWND_TOPMOST 越过全部外部窗口置顶呈现，再通过跨线程输入队列桥接解除 Windows Foreground Lock，
    // 最后恢复为 HWND_NOTOPMOST 普通顶层最前窗口（既无需点击任务栏即可立即浮现，又不永久遮挡后续文件对话框）。
    SetWindowPos(m_hwnd, HWND_TOPMOST, posX, posY, fitted.cx, fitted.cy, SWP_SHOWWINDOW);
    tools3000::core::WinUtils::applyUniversalRoundedCorners(
        m_hwnd, fitted.cx, fitted.cy, tools3000::core::dpi::scaleMetric(14, m_dpiScale));

    AllowSetForegroundWindow(ASFW_ANY);
    const HWND fgHwnd = GetForegroundWindow();
    const DWORD curTid = GetCurrentThreadId();
    const DWORD fgTid = fgHwnd ? GetWindowThreadProcessId(fgHwnd, nullptr) : 0;
    bool attached = false;
    if (fgHwnd && fgHwnd != m_hwnd && fgTid != 0 && fgTid != curTid) {
        attached = (AttachThreadInput(curTid, fgTid, TRUE) != FALSE);
    }
    ShowWindow(m_hwnd, SW_SHOW);
    BringWindowToTop(m_hwnd);
    SetForegroundWindow(m_hwnd);
    SetActiveWindow(m_hwnd);
    SetFocus(m_hwnd);
    if (attached) {
        AttachThreadInput(curTid, fgTid, FALSE);
    }
    SetWindowPos(m_hwnd, HWND_NOTOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_SHOWWINDOW);

    SetTimer(m_hwnd, kPlaybackTimerId, kTimerIntervalMs, nullptr);
    InvalidateRect(m_hwnd, nullptr, FALSE);

    LOG_INFO("StudioPreviewWindow opened: raw={}, deckFrames={}, zoomSegments={}, wallpapers={}",
             m_rawVideoPath, m_previewDeckBgr.size(),
             m_timeline.zoomSegments.size(), m_allWallpapers.size());
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

    const double srcSec = currentDisplayedSourceSec();
    const double norm = (m_durationSec > 1e-4)
        ? std::clamp(srcSec / m_durationSec, 0.0, 1.0)
        : 0.0;
    const size_t deckIdx = (std::min)(
        m_previewDeckBgr.size() - 1,
        static_cast<size_t>(std::llround(norm * static_cast<double>(m_previewDeckBgr.size() - 1))));
    const size_t evalIdx = m_evaluatedFrames.empty()
        ? 0
        : (std::min)(
            m_evaluatedFrames.size() - 1,
            static_cast<size_t>(std::llround(srcSec * m_previewEvalFps)));

    const StudioEvaluatedFrame fallbackEval{};
    const StudioEvaluatedFrame& eval = m_evaluatedFrames.empty() ? fallbackEval : m_evaluatedFrames[evalIdx];
    m_lastEvalCamera = eval.camera;

    const cv::Mat& srcFrame = m_previewDeckBgr[deckIdx];
    const cv::Size targetSz = (m_config.aspectRatio == StudioAspectRatio::Original)
        ? cv::Size(srcFrame.cols, srcFrame.rows)
        : StudioVideoCompositor::resolveOutputCanvasSize(srcFrame.cols, srcFrame.rows, m_config.aspectRatio);

    const int outW = targetSz.width;
    const int outH = targetSz.height;
    const bool hasAspectPad = (m_config.aspectRatio != StudioAspectRatio::Original);
    if (m_config.autoFrameEnabled || hasAspectPad) {
        const float safeRatio = m_config.autoFrameEnabled
            ? std::clamp(m_config.paddingRatio, 0.0f, 0.18f)
            : 0.0f;
        const int basePad = (safeRatio > 0.002f)
            ? (std::max)(8, static_cast<int>(std::round((std::min)(outW, outH) * safeRatio)))
            : 0;
        const int availW = (std::max)(32, outW - basePad * 2);
        const int availH = (std::max)(32, outH - basePad * 2);
        const float scaleFit = (std::min)(
            static_cast<float>(availW) / static_cast<float>((std::max)(1, srcFrame.cols)),
            static_cast<float>(availH) / static_cast<float>((std::max)(1, srcFrame.rows)));
        m_lastInnerW = (std::max)(16, static_cast<int>(std::round(srcFrame.cols * scaleFit)));
        m_lastInnerH = (std::max)(16, static_cast<int>(std::round(srcFrame.rows * scaleFit)));
        m_lastInnerPadX = (outW - m_lastInnerW) / 2;
        m_lastInnerPadY = (outH - m_lastInnerH) / 2;
    } else {
        m_lastInnerPadX = 0;
        m_lastInnerPadY = 0;
        m_lastInnerW = outW;
        m_lastInnerH = outH;
    }

    const auto* annPtr = m_timeline.annotations.empty() ? nullptr : &m_timeline.annotations;
    cv::Mat composedBgr = m_previewCompositor.composeFrame(srcFrame, eval, m_config, targetSz, annPtr);
    if (!composedBgr.empty()) {
        cv::cvtColor(composedBgr, m_currentComposedBgra, cv::COLOR_BGR2BGRA);
        m_composedDirty = true;
    }
}

bool StudioPreviewWindow::mapStagePointToSourceNorm(float lx, float ly, float& outNormX, float& outNormY) const {
    const float drawW = m_lastBitmapDrawRect.right - m_lastBitmapDrawRect.left;
    const float drawH = m_lastBitmapDrawRect.bottom - m_lastBitmapDrawRect.top;
    if (drawW <= 4.0f || drawH <= 4.0f || m_bitmapW == 0 || m_bitmapH == 0 ||
        m_lastInnerW <= 2 || m_lastInnerH <= 2) {
        return false;
    }
    const float bx = ((lx - m_lastBitmapDrawRect.left) / drawW) * static_cast<float>(m_bitmapW);
    const float by = ((ly - m_lastBitmapDrawRect.top) / drawH) * static_cast<float>(m_bitmapH);
    const float u = std::clamp((bx - static_cast<float>(m_lastInnerPadX)) / static_cast<float>(m_lastInnerW), 0.0f, 1.0f);
    const float v = std::clamp((by - static_cast<float>(m_lastInnerPadY)) / static_cast<float>(m_lastInnerH), 0.0f, 1.0f);

    const float srcW = static_cast<float>((std::max)(1, m_timeline.sourceWidth));
    const float srcH = static_cast<float>((std::max)(1, m_timeline.sourceHeight));
    if (m_lastEvalCamera.cropW > 0.5f && m_lastEvalCamera.cropH > 0.5f) {
        outNormX = std::clamp((m_lastEvalCamera.cropX + u * m_lastEvalCamera.cropW) / srcW, 0.0f, 1.0f);
        outNormY = std::clamp((m_lastEvalCamera.cropY + v * m_lastEvalCamera.cropH) / srcH, 0.0f, 1.0f);
    } else {
        outNormX = u;
        outNormY = v;
    }
    return true;
}

bool StudioPreviewWindow::mapSourceNormToStagePoint(float normX, float normY, float& outLx, float& outLy) const {
    const float drawW = m_lastBitmapDrawRect.right - m_lastBitmapDrawRect.left;
    const float drawH = m_lastBitmapDrawRect.bottom - m_lastBitmapDrawRect.top;
    if (drawW <= 4.0f || drawH <= 4.0f || m_bitmapW == 0 || m_bitmapH == 0 ||
        m_lastInnerW <= 2 || m_lastInnerH <= 2) {
        return false;
    }
    const float srcW = static_cast<float>((std::max)(1, m_timeline.sourceWidth));
    const float srcH = static_cast<float>((std::max)(1, m_timeline.sourceHeight));
    const float sx = normX * srcW;
    const float sy = normY * srcH;
    const float u = (m_lastEvalCamera.cropW > 0.5f) ? ((sx - m_lastEvalCamera.cropX) / m_lastEvalCamera.cropW) : normX;
    const float v = (m_lastEvalCamera.cropH > 0.5f) ? ((sy - m_lastEvalCamera.cropY) / m_lastEvalCamera.cropH) : normY;

    const float bx = static_cast<float>(m_lastInnerPadX) + u * static_cast<float>(m_lastInnerW);
    const float by = static_cast<float>(m_lastInnerPadY) + v * static_cast<float>(m_lastInnerH);
    outLx = m_lastBitmapDrawRect.left + (bx / static_cast<float>(m_bitmapW)) * drawW;
    outLy = m_lastBitmapDrawRect.top + (by / static_cast<float>(m_bitmapH)) * drawH;
    return true;
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
            DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, 15.0f, L"zh-CN", m_titleFmt.GetAddressOf());
        m_dwriteFactory->CreateTextFormat(
            uiFont, nullptr, DWRITE_FONT_WEIGHT_SEMI_BOLD,
            DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, 12.8f, L"zh-CN", m_sectionFmt.GetAddressOf());
        m_dwriteFactory->CreateTextFormat(
            uiFont, nullptr, DWRITE_FONT_WEIGHT_MEDIUM,
            DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, 12.1f, L"zh-CN", m_bodyFmt.GetAddressOf());
        m_dwriteFactory->CreateTextFormat(
            uiFont, nullptr, DWRITE_FONT_WEIGHT_MEDIUM,
            DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, 11.9f, L"zh-CN", m_captionFmt.GetAddressOf());
        m_dwriteFactory->CreateTextFormat(
            monoFont, nullptr, DWRITE_FONT_WEIGHT_SEMI_BOLD,
            DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, 11.9f, L"en-US", m_monoFmt.GetAddressOf());

        ComPtr<IDWriteInlineObject> ellipsis;
        if (m_bodyFmt && SUCCEEDED(m_dwriteFactory->CreateEllipsisTrimmingSign(m_bodyFmt.Get(), ellipsis.GetAddressOf()))) {
            DWRITE_TRIMMING trim{DWRITE_TRIMMING_GRANULARITY_CHARACTER, 0, 0};
            m_bodyFmt->SetTrimming(&trim, ellipsis.Get());
            m_bodyFmt->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
        }
        if (m_captionFmt) {
            m_captionFmt->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
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
        m_wallpaperThumbsDirty = true;
    }

    if (!m_wallpapersDiscovered) {
        discoverLocalWallpapers();
    }

    if (m_wallpaperThumbsDirty && m_renderTarget) {
        D2D1_BITMAP_PROPERTIES bp = D2D1::BitmapProperties(
            D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_IGNORE),
            96.0f, 96.0f);
        const size_t startIdx = static_cast<size_t>(m_wallpaperPage) * kWallpaperSlotsPerPage;
        for (size_t slot = 0; slot < kWallpaperSlotsPerPage; ++slot) {
            m_themeThumbBitmaps[slot].Reset();
            const size_t fltPos = startIdx + slot;
            if (fltPos >= m_filteredWallpaperIndices.size()) continue;
            const auto& entry = m_allWallpapers[static_cast<size_t>(m_filteredWallpaperIndices[fltPos])];
            cv::Mat thumb = entry.isBuiltin
                ? StudioVideoCompositor::renderThemeThumbnailBgra(entry.builtinTheme, 64, 64)
                : StudioVideoCompositor::renderCustomWallpaperThumbnailBgra(entry.filePathUtf8, 64, 64);
            if (!thumb.empty()) {
                m_renderTarget->CreateBitmap(
                    D2D1::SizeU(static_cast<UINT32>(thumb.cols), static_cast<UINT32>(thumb.rows)),
                    thumb.data,
                    static_cast<UINT32>(thumb.step[0]),
                    &bp,
                    m_themeThumbBitmaps[slot].GetAddressOf());
            }
        }
        m_wallpaperThumbsDirty = false;
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
    m_wallpaperThumbsDirty = true;
}

void StudioPreviewWindow::insertAnnotationCardPreset(int cardIndex) {
    m_playing = false;
    pushUndoSnapshot();

    const int64_t rawTotalMs = (std::max<int64_t>)(400, static_cast<int64_t>(std::llround(m_durationSec * 1000.0)));
    const int64_t curSrcMs = static_cast<int64_t>(std::llround(currentDisplayedSourceSec() * 1000.0));
    const int64_t stMs = std::clamp<int64_t>(curSrcMs, 0, rawTotalMs - 200);
    const int64_t edMs = (std::min)(rawTotalMs, stMs + 3000);
    const bool isZh = tools3000::core::NativeThemePalette::isUiLanguageChinese();

    if (cardIndex == 18) {
        StudioZoomSegment z{};
        z.startMs = stMs;
        z.endMs = edMs;
        z.focusX = static_cast<float>(m_timeline.sourceWidth) * 0.5f;
        z.focusY = static_cast<float>(m_timeline.sourceHeight) * 0.5f;
        z.targetZoom = (std::max)(1.65f, m_config.defaultZoomFactor);
        z.enabled = true;
        z.manualInserted = true;
        m_config.autoZoomEnabled = true;
        if (m_config.zoomMode == StudioZoomMode::None) {
            m_config.zoomMode = StudioZoomMode::Zoom2D;
        }
        m_timeline.zoomSegments.push_back(z);
        m_selectedZoomIdx = static_cast<int>(m_timeline.zoomSegments.size()) - 1;
        m_selectedClipIdx = -1;
        m_selectedAnnIdx = -1;
        rebakeTimelinePreview();
        requestImmediatePreviewRefresh(true);
        return;
    }

    StudioAnnotationItem ann{};
    ann.startMs = stMs;
    ann.endMs = edMs;
    ann.x0 = 0.36f;
    ann.y0 = 0.38f;
    ann.x1 = 0.64f;
    ann.y1 = 0.58f;
    ann.strokeWidth = 3.5f;
    ann.enabled = true;

    switch (cardIndex) {
        case 0:
            ann.kind = StudioAnnotationKind::TextPlain;
            ann.colorHex = "#F59E0B";
            ann.text = isZh ? "重点说明" : "Text";
            break;
        case 1:
            ann.kind = StudioAnnotationKind::Text;
            ann.colorHex = "#1E293B";
            ann.text = isZh ? "操作提示" : "Text";
            break;
        case 2:
            ann.kind = StudioAnnotationKind::TextBubble;
            ann.colorHex = "#A855F7";
            ann.text = isZh ? "点击这里" : "Text";
            break;
        case 3:
            ann.kind = StudioAnnotationKind::TextDashed;
            ann.colorHex = "#94A3B8";
            ann.text = isZh ? "补充备注" : "Text";
            break;
        case 4:
            ann.kind = StudioAnnotationKind::TextNeon;
            ann.colorHex = "#EC4899";
            ann.text = isZh ? "关键步骤" : "Text";
            break;
        case 5:
            ann.kind = StudioAnnotationKind::StepBadge;
            ann.colorHex = "#3B82F6";
            ann.text = std::to_string(m_nextStepBadgeNumber++);
            ann.x0 = 0.46f; ann.y0 = 0.44f; ann.x1 = 0.54f; ann.y1 = 0.52f;
            break;
        case 6:
            ann.kind = StudioAnnotationKind::GlowLine;
            ann.colorHex = "#10B981";
            ann.x0 = 0.32f; ann.y0 = 0.52f; ann.x1 = 0.68f; ann.y1 = 0.52f;
            break;
        case 7:
            ann.kind = StudioAnnotationKind::DashedLine;
            ann.colorHex = "#38BDF8";
            ann.x0 = 0.32f; ann.y0 = 0.50f; ann.x1 = 0.68f; ann.y1 = 0.50f;
            break;
        case 8:
            ann.kind = StudioAnnotationKind::Arrow;
            ann.colorHex = "#F43F5E";
            ann.x0 = 0.34f; ann.y0 = 0.38f; ann.x1 = 0.56f; ann.y1 = 0.54f;
            break;
        case 9:
            ann.kind = StudioAnnotationKind::Rectangle;
            ann.colorHex = "#EF4444";
            break;
        case 10:
            ann.kind = StudioAnnotationKind::RoundedBox;
            ann.colorHex = "#F59E0B";
            break;
        case 11:
            ann.kind = StudioAnnotationKind::CircleRing;
            ann.colorHex = "#FFFFFF";
            ann.x0 = 0.40f; ann.y0 = 0.36f; ann.x1 = 0.60f; ann.y1 = 0.64f;
            break;
        case 12:
            ann.kind = StudioAnnotationKind::SketchCircle;
            ann.colorHex = "#F97316";
            ann.x0 = 0.38f; ann.y0 = 0.38f; ann.x1 = 0.62f; ann.y1 = 0.60f;
            break;
        case 13:
            ann.kind = StudioAnnotationKind::BoldArrow;
            ann.colorHex = "#EC4899";
            ann.x0 = 0.50f; ann.y0 = 0.28f;
            ann.x1 = 0.50f; ann.y1 = 0.48f;
            ann.strokeWidth = 5.0f;
            break;
        case 14:
            ann.kind = StudioAnnotationKind::HandPointer;
            ann.colorHex = "#EC4899";
            ann.x0 = 0.46f; ann.y0 = 0.45f; ann.x1 = 0.54f; ann.y1 = 0.55f;
            break;
        case 15:
            ann.kind = StudioAnnotationKind::Spotlight;
            ann.colorHex = "#FFFFFF";
            ann.x0 = 0.30f; ann.y0 = 0.32f; ann.x1 = 0.70f; ann.y1 = 0.66f;
            break;
        case 16:
            ann.kind = StudioAnnotationKind::BlurMask;
            ann.colorHex = "#94A3B8";
            ann.x0 = 0.34f; ann.y0 = 0.38f; ann.x1 = 0.66f; ann.y1 = 0.56f;
            break;
        case 17:
        default:
            ann.kind = StudioAnnotationKind::Magnifier;
            ann.colorHex = "#FFFFFF";
            ann.x0 = 0.40f; ann.y0 = 0.36f; ann.x1 = 0.60f; ann.y1 = 0.64f;
            break;
    }

    m_timeline.annotations.push_back(ann);
    m_selectedAnnIdx = static_cast<int>(m_timeline.annotations.size()) - 1;
    m_selectedZoomIdx = -1;
    m_selectedClipIdx = -1;
    m_editingAnnotationText = (cardIndex >= 0 && cardIndex <= 4);

    requestImmediatePreviewRefresh(true);
}

void StudioPreviewWindow::renderAnnotationDrawer(
    float padL, float padR, float startY, float bottomLimit,
    bool dark, bool isZh, ID2D1SolidColorBrush* brush) {
    const auto accentRgb = tools3000::core::NativeThemePalette::getEffectiveAccentColorRGB();
    const D2D1_COLOR_F colAccent = D2D1::ColorF(accentRgb.r, accentRgb.g, accentRgb.b, 1.0f);
    const D2D1_COLOR_F colPrimary = dark ? D2D1::ColorF(0.96f, 0.97f, 0.99f, 1.0f) : D2D1::ColorF(0.06f, 0.09f, 0.16f, 1.0f);
    const D2D1_COLOR_F colSecondary = dark ? D2D1::ColorF(0.64f, 0.70f, 0.80f, 1.0f) : D2D1::ColorF(0.35f, 0.41f, 0.51f, 1.0f);
    const D2D1_COLOR_F colTileBg = dark ? D2D1::ColorF(0.125f, 0.145f, 0.205f, 1.0f) : D2D1::ColorF(0.935f, 0.950f, 0.975f, 1.0f);
    const D2D1_COLOR_F colBorder = dark ? D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.12f) : D2D1::ColorF(0.06f, 0.09f, 0.16f, 0.12f);

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

    const float fullW = padR - padL;
    float y = startY;

    drawTxt(isZh ? L"文本" : L"Text", m_sectionFmt.Get(), D2D1::RectF(padL, y, padR, y + 18.0f), colPrimary);
    y += 21.0f;
    {
        const float gap = 6.0f;
        const float cw = (fullW - gap * 2.0f) / 3.0f;
        const float ch = 36.0f;
        for (int i = 0; i < 6; ++i) {
            const int col = i % 3;
            const int row = i / 3;
            const float tx = padL + col * (cw + gap);
            const float ty = y + row * (ch + gap);
            const D2D1_RECT_F rc = D2D1::RectF(tx, ty, tx + cw, ty + ch);
            const int hId = Hit_AnnCardBase + i;
            m_hitZones.push_back({hId, rc, i});

            setBrush(colTileBg);
            m_renderTarget->FillRoundedRectangle(D2D1::RoundedRect(rc, 6.5f, 6.5f), brush);
            setBrush(m_hoveredHitId == hId ? colAccent : colBorder);
            m_renderTarget->DrawRoundedRectangle(D2D1::RoundedRect(rc, 6.5f, 6.5f), brush, m_hoveredHitId == hId ? 1.5f : 0.9f);

            const D2D1_RECT_F inner = D2D1::RectF(rc.left + 9.0f, rc.top + 6.5f, rc.right - 9.0f, rc.bottom - 6.5f);
            if (i == 0) {
                drawTxt(L"Text", m_bodyFmt.Get(), rc, colPrimary, DWRITE_TEXT_ALIGNMENT_CENTER);
            } else if (i == 1) {
                setBrush(D2D1::ColorF(0.18f, 0.22f, 0.30f, 1.0f));
                m_renderTarget->FillRoundedRectangle(D2D1::RoundedRect(inner, 6.0f, 6.0f), brush);
                drawTxt(L"Text", m_captionFmt.Get(), inner, D2D1::ColorF(1, 1, 1, 1), DWRITE_TEXT_ALIGNMENT_CENTER);
            } else if (i == 2) {
                const D2D1_RECT_F brc = D2D1::RectF(inner.left, inner.top - 1.0f, inner.right, inner.bottom - 3.0f);
                setBrush(D2D1::ColorF(0.66f, 0.33f, 0.97f, 1.0f));
                m_renderTarget->FillRoundedRectangle(D2D1::RoundedRect(brc, 7.0f, 7.0f), brush);
                const float bcx = (brc.left + brc.right) * 0.5f;
                m_renderTarget->DrawLine({bcx - 3.0f, brc.bottom}, {bcx, brc.bottom + 4.0f}, brush, 2.2f);
                m_renderTarget->DrawLine({bcx + 3.0f, brc.bottom}, {bcx, brc.bottom + 4.0f}, brush, 2.2f);
                drawTxt(L"Text", m_captionFmt.Get(), brc, D2D1::ColorF(1, 1, 1, 1), DWRITE_TEXT_ALIGNMENT_CENTER);
            } else if (i == 3) {
                D2D1_STROKE_STYLE_PROPERTIES sp = D2D1::StrokeStyleProperties();
                sp.dashStyle = D2D1_DASH_STYLE_DASH;
                ComPtr<ID2D1StrokeStyle> ds;
                m_d2dFactory->CreateStrokeStyle(sp, nullptr, 0, ds.GetAddressOf());
                setBrush(colSecondary);
                m_renderTarget->DrawRoundedRectangle(D2D1::RoundedRect(inner, 5.0f, 5.0f), brush, 1.3f, ds.Get());
                drawTxt(L"Text", m_captionFmt.Get(), inner, colPrimary, DWRITE_TEXT_ALIGNMENT_CENTER);
            } else if (i == 4) {
                setBrush(D2D1::ColorF(0.93f, 0.28f, 0.60f, 0.35f));
                m_renderTarget->DrawRoundedRectangle(D2D1::RoundedRect(inner, 5.5f, 5.5f), brush, 3.4f);
                setBrush(D2D1::ColorF(0.93f, 0.28f, 0.60f, 1.0f));
                m_renderTarget->DrawRoundedRectangle(D2D1::RoundedRect(inner, 5.5f, 5.5f), brush, 1.4f);
                drawTxt(L"Text", m_captionFmt.Get(), inner, colPrimary, DWRITE_TEXT_ALIGNMENT_CENTER);
            } else {
                const float cx = (rc.left + rc.right) * 0.5f;
                const float cy = (rc.top + rc.bottom) * 0.5f;
                setBrush(D2D1::ColorF(0.23f, 0.51f, 0.96f, 1.0f));
                m_renderTarget->FillEllipse(D2D1::Ellipse({cx, cy}, 11.5f, 11.5f), brush);
                drawTxt(L"1", m_bodyFmt.Get(), rc, D2D1::ColorF(1, 1, 1, 1), DWRITE_TEXT_ALIGNMENT_CENTER);
            }
        }
        y += ch * 2.0f + gap + 9.0f;
    }

    drawTxt(isZh ? L"形状" : L"Shapes", m_sectionFmt.Get(), D2D1::RectF(padL, y, padR, y + 18.0f), colPrimary);
    y += 21.0f;
    {
        const float gap = 6.0f;
        const float cw = (fullW - gap * 2.0f) / 3.0f;
        const float ch = 40.0f;
        for (int i = 0; i < 9; ++i) {
            const int cardIdx = 6 + i;
            const int col = i % 3;
            const int row = i / 3;
            const float tx = padL + col * (cw + gap);
            const float ty = y + row * (ch + gap);
            const D2D1_RECT_F rc = D2D1::RectF(tx, ty, tx + cw, ty + ch);
            const int hId = Hit_AnnCardBase + cardIdx;
            m_hitZones.push_back({hId, rc, cardIdx});

            setBrush(colTileBg);
            m_renderTarget->FillRoundedRectangle(D2D1::RoundedRect(rc, 6.5f, 6.5f), brush);
            setBrush(m_hoveredHitId == hId ? colAccent : colBorder);
            m_renderTarget->DrawRoundedRectangle(D2D1::RoundedRect(rc, 6.5f, 6.5f), brush, m_hoveredHitId == hId ? 1.5f : 0.9f);

            const float cx = (rc.left + rc.right) * 0.5f;
            const float cy = (rc.top + rc.bottom) * 0.5f;
            if (i == 0) {
                setBrush(D2D1::ColorF(0.06f, 0.73f, 0.51f, 0.32f));
                m_renderTarget->DrawLine({cx - 18.0f, cy}, {cx + 18.0f, cy}, brush, 5.5f);
                setBrush(D2D1::ColorF(0.06f, 0.73f, 0.51f, 1.0f));
                m_renderTarget->DrawLine({cx - 18.0f, cy}, {cx + 18.0f, cy}, brush, 2.4f);
            } else if (i == 1) {
                D2D1_STROKE_STYLE_PROPERTIES sp = D2D1::StrokeStyleProperties();
                sp.dashStyle = D2D1_DASH_STYLE_DASH;
                ComPtr<ID2D1StrokeStyle> ds;
                m_d2dFactory->CreateStrokeStyle(sp, nullptr, 0, ds.GetAddressOf());
                setBrush(D2D1::ColorF(0.22f, 0.74f, 0.97f, 1.0f));
                m_renderTarget->DrawLine({cx - 18.0f, cy}, {cx + 18.0f, cy}, brush, 2.4f, ds.Get());
            } else if (i == 2) {
                setBrush(D2D1::ColorF(0.96f, 0.25f, 0.37f, 1.0f));
                m_renderTarget->DrawLine({cx - 18.0f, cy}, {cx + 16.0f, cy}, brush, 2.3f);
                m_renderTarget->DrawLine({cx + 9.0f, cy - 5.5f}, {cx + 17.0f, cy}, brush, 2.3f);
                m_renderTarget->DrawLine({cx + 9.0f, cy + 5.5f}, {cx + 17.0f, cy}, brush, 2.3f);
            } else if (i == 3) {
                setBrush(D2D1::ColorF(0.94f, 0.27f, 0.27f, 1.0f));
                m_renderTarget->DrawRectangle(D2D1::RectF(cx - 10.5f, cy - 10.5f, cx + 10.5f, cy + 10.5f), brush, 2.4f);
            } else if (i == 4) {
                setBrush(D2D1::ColorF(0.96f, 0.62f, 0.04f, 1.0f));
                m_renderTarget->DrawRoundedRectangle(
                    D2D1::RoundedRect(D2D1::RectF(cx - 10.5f, cy - 10.5f, cx + 10.5f, cy + 10.5f), 5.0f, 5.0f), brush, 2.4f);
            } else if (i == 5) {
                setBrush(colPrimary);
                m_renderTarget->DrawEllipse(D2D1::Ellipse({cx, cy}, 10.5f, 10.5f), brush, 2.6f);
            } else if (i == 6) {
                setBrush(D2D1::ColorF(0.98f, 0.45f, 0.09f, 1.0f));
                m_renderTarget->DrawEllipse(D2D1::Ellipse({cx - 1.0f, cy}, 13.0f, 8.5f), brush, 2.0f);
                m_renderTarget->DrawEllipse(D2D1::Ellipse({cx + 1.5f, cy - 1.0f}, 12.0f, 9.5f), brush, 1.4f);
            } else if (i == 7) {
                setBrush(D2D1::ColorF(0.93f, 0.28f, 0.60f, 1.0f));
                ComPtr<ID2D1PathGeometry> geo;
                if (SUCCEEDED(m_d2dFactory->CreatePathGeometry(geo.GetAddressOf())) && geo) {
                    ComPtr<ID2D1GeometrySink> sk;
                    if (SUCCEEDED(geo->Open(sk.GetAddressOf())) && sk) {
                        sk->BeginFigure({cx - 3.8f, cy - 12.5f}, D2D1_FIGURE_BEGIN_FILLED);
                        sk->AddLine({cx + 3.8f, cy - 12.5f});
                        sk->AddLine({cx + 3.8f, cy + 1.0f});
                        sk->AddLine({cx + 9.5f, cy + 1.0f});
                        sk->AddLine({cx, cy + 12.5f});
                        sk->AddLine({cx - 9.5f, cy + 1.0f});
                        sk->AddLine({cx - 3.8f, cy + 1.0f});
                        sk->EndFigure(D2D1_FIGURE_END_CLOSED);
                        sk->Close();
                        m_renderTarget->FillGeometry(geo.Get(), brush);
                    }
                }
            } else {
                setBrush(colPrimary);
                m_renderTarget->FillRoundedRectangle(
                    D2D1::RoundedRect(D2D1::RectF(cx - 2.5f, cy - 11.5f, cx + 2.5f, cy + 3.0f), 2.5f, 2.5f), brush);
                m_renderTarget->FillRoundedRectangle(
                    D2D1::RoundedRect(D2D1::RectF(cx - 6.5f, cy - 2.0f, cx + 8.5f, cy + 11.5f), 5.0f, 5.0f), brush);
            }
        }
        y += ch * 3.0f + gap * 2.0f + 9.0f;
    }

    drawTxt(isZh ? L"遮罩" : L"Mask & Zoom", m_sectionFmt.Get(), D2D1::RectF(padL, y, padR, y + 18.0f), colPrimary);
    y += 21.0f;
    {
        static const wchar_t* maskZh[4] = { L"聚光灯", L"马赛克", L"放大镜", L"局部缩放" };
        static const wchar_t* maskEn[4] = { L"Spot", L"Mosaic", L"Loupe", L"Zoom" };
        const float gap = 5.5f;
        const float cw = (fullW - gap * 3.0f) / 4.0f;
        const float ch = 48.0f;
        for (int m = 0; m < 4; ++m) {
            const int cardIdx = 15 + m;
            const float tx = padL + m * (cw + gap);
            const D2D1_RECT_F rc = D2D1::RectF(tx, y, tx + cw, y + ch);
            const int hId = Hit_AnnCardBase + cardIdx;
            m_hitZones.push_back({hId, rc, cardIdx});

            setBrush(colTileBg);
            m_renderTarget->FillRoundedRectangle(D2D1::RoundedRect(rc, 6.5f, 6.5f), brush);
            setBrush(m_hoveredHitId == hId ? colAccent : colBorder);
            m_renderTarget->DrawRoundedRectangle(D2D1::RoundedRect(rc, 6.5f, 6.5f), brush, m_hoveredHitId == hId ? 1.5f : 0.9f);

            const float cx = (rc.left + rc.right) * 0.5f;
            const float iy = rc.top + 15.0f;
            setBrush(colPrimary);
            if (m == 0) {
                m_renderTarget->DrawRoundedRectangle(
                    D2D1::RoundedRect(D2D1::RectF(cx - 7.0f, iy - 6.0f, cx + 7.0f, iy - 1.0f), 2.0f, 2.0f), brush, 1.5f);
                m_renderTarget->DrawLine({cx - 5.0f, iy + 1.0f}, {cx - 9.0f, iy + 8.0f}, brush, 1.3f);
                m_renderTarget->DrawLine({cx, iy + 1.0f}, {cx, iy + 8.5f}, brush, 1.3f);
                m_renderTarget->DrawLine({cx + 5.0f, iy + 1.0f}, {cx + 9.0f, iy + 8.0f}, brush, 1.3f);
            } else if (m == 1) {
                for (int gy = 0; gy < 3; ++gy) {
                    for (int gx = 0; gx < 3; ++gx) {
                        if (((gx + gy) & 1) == 0) {
                            m_renderTarget->FillRectangle(
                                D2D1::RectF(cx - 7.5f + gx * 5.0f, iy - 7.0f + gy * 5.0f,
                                            cx - 3.0f + gx * 5.0f, iy - 2.5f + gy * 5.0f), brush);
                        }
                    }
                }
            } else if (m == 2) {
                m_renderTarget->DrawEllipse(D2D1::Ellipse({cx - 2.0f, iy - 1.5f}, 6.5f, 6.5f), brush, 1.6f);
                m_renderTarget->DrawLine({cx + 3.0f, iy + 3.5f}, {cx + 8.5f, iy + 9.0f}, brush, 2.0f);
                m_renderTarget->DrawLine({cx - 4.5f, iy - 1.5f}, {cx + 0.5f, iy - 1.5f}, brush, 1.3f);
                m_renderTarget->DrawLine({cx - 2.0f, iy - 4.0f}, {cx - 2.0f, iy + 1.0f}, brush, 1.3f);
            } else {
                m_renderTarget->DrawRoundedRectangle(
                    D2D1::RoundedRect(D2D1::RectF(cx - 9.0f, iy - 6.5f, cx + 9.0f, iy + 6.5f), 2.5f, 2.5f), brush, 1.5f);
                m_renderTarget->FillRoundedRectangle(
                    D2D1::RoundedRect(D2D1::RectF(cx - 4.0f, iy - 2.5f, cx + 5.0f, iy + 3.5f), 1.5f, 1.5f), brush);
            }
            drawTxt(isZh ? maskZh[m] : maskEn[m], m_captionFmt.Get(),
                    D2D1::RectF(rc.left, rc.bottom - 18.0f, rc.right, rc.bottom - 2.0f),
                    colSecondary, DWRITE_TEXT_ALIGNMENT_CENTER);
        }
        y += ch + 10.0f;
    }

    setBrush(colBorder);
    m_renderTarget->DrawLine({padL, y}, {padR, y}, brush, 1.0f);
    y += 8.0f;

    if (m_selectedAnnIdx >= 0 && m_selectedAnnIdx < static_cast<int>(m_timeline.annotations.size()) &&
        y + 52.0f <= bottomLimit) {
        auto& ann = m_timeline.annotations[static_cast<size_t>(m_selectedAnnIdx)];
        drawTxt(isZh ? L"选中标注样式 (可在右侧画面直接拖拽移位)" : L"Selected Style (Drag on stage to move)",
                m_captionFmt.Get(), D2D1::RectF(padL, y, padR, y + 18.0f), colSecondary);
        y += 22.0f;

        const float dotStep = fullW / static_cast<float>(kAnnotationPaletteHex.size());
        for (size_t c = 0; c < kAnnotationPaletteHex.size(); ++c) {
            const float dcx = padL + dotStep * (static_cast<float>(c) + 0.5f);
            const float dcy = y + 10.0f;
            const D2D1_RECT_F hitRc = D2D1::RectF(dcx - 11.0f, dcy - 11.0f, dcx + 11.0f, dcy + 11.0f);
            m_hitZones.push_back({Hit_AnnColorBase + static_cast<int>(c), hitRc, static_cast<int>(c)});
            setBrush(hexToD2DColor(kAnnotationPaletteHex[c]));
            m_renderTarget->FillEllipse(D2D1::Ellipse({dcx, dcy}, 8.5f, 8.5f), brush);
            setBrush(ann.colorHex == kAnnotationPaletteHex[c] ? colAccent : colBorder);
            m_renderTarget->DrawEllipse(D2D1::Ellipse({dcx, dcy}, 10.2f, 10.2f), brush,
                                        ann.colorHex == kAnnotationPaletteHex[c] ? 2.0f : 1.0f);
        }
        y += 26.0f;

        if (isTextLikeAnnotation(ann.kind) && y + 30.0f <= bottomLimit) {
            const D2D1_RECT_F txtBox = D2D1::RectF(padL, y, padR, y + 28.0f);
            m_hitZones.push_back({Hit_AnnTextInputBox, txtBox, -1});
            setBrush(colTileBg);
            m_renderTarget->FillRoundedRectangle(D2D1::RoundedRect(txtBox, 6.0f, 6.0f), brush);
            setBrush(m_editingAnnotationText ? colAccent : colBorder);
            m_renderTarget->DrawRoundedRectangle(D2D1::RoundedRect(txtBox, 6.0f, 6.0f), brush, m_editingAnnotationText ? 1.5f : 1.0f);

            std::wstring shown = tools3000::core::WinUtils::utf8ToWstring(ann.text);
            if (m_editingAnnotationText && ((GetTickCount() / 500) % 2 == 0)) {
                shown.push_back(L'|');
            }
            drawTxt(shown.empty() ? (isZh ? L"点击输入文字..." : L"Click to type...") : shown,
                    m_bodyFmt.Get(), D2D1::RectF(txtBox.left + 8.0f, txtBox.top, txtBox.right - 8.0f, txtBox.bottom), colPrimary);
        }
    } else if (y + 24.0f <= bottomLimit) {
        drawTxt(isZh ? L"点击任意卡片即可添加到当前时间画面" : L"Click any style card to place on video",
                m_captionFmt.Get(), D2D1::RectF(padL, y, padR, y + 22.0f), colSecondary);
    }
}

void StudioPreviewWindow::renderLeftRailAndDrawer(
    const D2D1_RECT_F& railRc, const D2D1_RECT_F& drawerRc,
    bool dark, bool isZh, ID2D1SolidColorBrush* brush) {
    const auto accentRgb = tools3000::core::NativeThemePalette::getEffectiveAccentColorRGB();
    const D2D1_COLOR_F colAccent = D2D1::ColorF(accentRgb.r, accentRgb.g, accentRgb.b, 1.0f);
    const D2D1_COLOR_F colAccentSoft = D2D1::ColorF(accentRgb.r, accentRgb.g, accentRgb.b, dark ? 0.24f : 0.14f);
    const D2D1_COLOR_F colCardBg = dark ? D2D1::ColorF(0.092f, 0.114f, 0.165f, 1.0f) : D2D1::ColorF(1.0f, 1.0f, 1.0f, 1.0f);
    const D2D1_COLOR_F colRailBg = dark ? D2D1::ColorF(0.072f, 0.088f, 0.130f, 1.0f) : D2D1::ColorF(0.945f, 0.958f, 0.980f, 1.0f);
    const D2D1_COLOR_F colBorder = dark ? D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.10f) : D2D1::ColorF(0.059f, 0.090f, 0.165f, 0.11f);
    const D2D1_COLOR_F colPrimary = dark ? D2D1::ColorF(0.96f, 0.97f, 0.99f, 1.0f) : D2D1::ColorF(0.059f, 0.090f, 0.165f, 1.0f);
    const D2D1_COLOR_F colSecondary = dark ? D2D1::ColorF(0.62f, 0.68f, 0.78f, 1.0f) : D2D1::ColorF(0.32f, 0.38f, 0.47f, 1.0f);
    const D2D1_COLOR_F colChipBg = dark ? D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.06f) : D2D1::ColorF(0.059f, 0.090f, 0.165f, 0.06f);

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

    setBrush(colRailBg);
    m_renderTarget->FillRoundedRectangle(D2D1::RoundedRect(railRc, 11.0f, 11.0f), brush);
    setBrush(colBorder);
    m_renderTarget->DrawRoundedRectangle(D2D1::RoundedRect(railRc, 11.0f, 11.0f), brush, 1.0f);

    static const wchar_t* tabShortZh[4] = { L"背景", L"标注", L"运镜", L"字幕" };
    static const wchar_t* tabShortEn[4] = { L"Canvas", L"Mark", L"Cam", L"Sub" };
    for (int t = 0; t < 4; ++t) {
        const float by = railRc.top + 10.0f + t * 56.0f;
        const D2D1_RECT_F brc = D2D1::RectF(railRc.left + 4.0f, by, railRc.right - 4.0f, by + 48.0f);
        const int hId = Hit_LeftRailTabBase + t;
        m_hitZones.push_back({hId, brc, t});

        const bool active = (static_cast<int>(m_activeTab) == t);
        if (active || m_hoveredHitId == hId) {
            setBrush(active ? colAccentSoft : colChipBg);
            m_renderTarget->FillRoundedRectangle(D2D1::RoundedRect(brc, 8.0f, 8.0f), brush);
        }
        const float icx = (brc.left + brc.right) * 0.5f;
        const float icy = brc.top + 15.0f;
        setBrush(active ? colAccent : colSecondary);
        if (t == 0) {
            m_renderTarget->DrawRoundedRectangle(
                D2D1::RoundedRect(D2D1::RectF(icx - 8.0f, icy - 6.5f, icx + 8.0f, icy + 6.5f), 2.5f, 2.5f), brush, 1.6f);
            m_renderTarget->FillEllipse(D2D1::Ellipse({icx - 3.0f, icy - 2.0f}, 1.6f, 1.6f), brush);
            m_renderTarget->DrawLine({icx - 6.0f, icy + 5.0f}, {icx - 1.0f, icy + 0.5f}, brush, 1.4f);
            m_renderTarget->DrawLine({icx - 1.0f, icy + 0.5f}, {icx + 6.0f, icy + 5.0f}, brush, 1.4f);
        } else if (t == 1) {
            m_renderTarget->DrawRoundedRectangle(
                D2D1::RoundedRect(D2D1::RectF(icx - 7.5f, icy - 6.5f, icx + 5.5f, icy + 6.5f), 2.5f, 2.5f), brush, 1.5f);
            m_renderTarget->DrawLine({icx + 1.0f, icy + 1.0f}, {icx + 8.0f, icy - 6.0f}, brush, 2.0f);
        } else if (t == 2) {
            m_renderTarget->DrawEllipse(D2D1::Ellipse({icx, icy}, 7.0f, 7.0f), brush, 1.6f);
            m_renderTarget->FillEllipse(D2D1::Ellipse({icx, icy}, 2.4f, 2.4f), brush);
        } else {
            m_renderTarget->DrawRoundedRectangle(
                D2D1::RoundedRect(D2D1::RectF(icx - 8.0f, icy - 6.0f, icx + 8.0f, icy + 6.0f), 2.5f, 2.5f), brush, 1.5f);
            m_renderTarget->DrawLine({icx - 4.5f, icy + 1.5f}, {icx + 4.5f, icy + 1.5f}, brush, 1.5f);
        }
        drawTxt(isZh ? tabShortZh[t] : tabShortEn[t], m_captionFmt.Get(),
                D2D1::RectF(brc.left, brc.bottom - 18.0f, brc.right, brc.bottom - 2.0f),
                active ? colAccent : colSecondary, DWRITE_TEXT_ALIGNMENT_CENTER);
    }

    setBrush(colCardBg);
    m_renderTarget->FillRoundedRectangle(D2D1::RoundedRect(drawerRc, 12.0f, 12.0f), brush);
    setBrush(colBorder);
    m_renderTarget->DrawRoundedRectangle(D2D1::RoundedRect(drawerRc, 12.0f, 12.0f), brush, 1.0f);

    const float padL = drawerRc.left + 13.0f;
    const float padR = drawerRc.right - 13.0f;
    const float fullW = padR - padL;
    float curY = drawerRc.top + 11.0f;

    auto drawTogglePill = [&](int hitId, float rightEdge, float topY, bool on) {
        const D2D1_RECT_F rc = D2D1::RectF(rightEdge - 36.0f, topY, rightEdge, topY + 19.0f);
        m_hitZones.push_back({hitId, rc, -1});
        setBrush(on ? colAccent : (dark ? D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.16f) : D2D1::ColorF(0.75f, 0.80f, 0.86f, 1.0f)));
        m_renderTarget->FillRoundedRectangle(D2D1::RoundedRect(rc, 9.5f, 9.5f), brush);
        const float knobX = on ? (rc.right - 9.5f) : (rc.left + 9.5f);
        setBrush(D2D1::ColorF(1.0f, 1.0f, 1.0f, 1.0f));
        m_renderTarget->FillEllipse(D2D1::Ellipse({knobX, topY + 9.5f}, 6.8f, 6.8f), brush);
    };

    auto drawOptionChip = [&](int hitId, const D2D1_RECT_F& rc, const std::wstring& label, bool active) {
        m_hitZones.push_back({hitId, rc, -1});
        setBrush(active ? colAccentSoft : colChipBg);
        m_renderTarget->FillRoundedRectangle(D2D1::RoundedRect(rc, 5.5f, 5.5f), brush);
        setBrush(active ? colAccent : colBorder);
        m_renderTarget->DrawRoundedRectangle(D2D1::RoundedRect(rc, 5.5f, 5.5f), brush, active ? 1.3f : 0.9f);
        drawTxt(label, m_captionFmt.Get(), rc, active ? colAccent : colSecondary, DWRITE_TEXT_ALIGNMENT_CENTER);
    };

    auto drawSliderRow = [&](SliderDragKind kind, int hitId, float topY,
                             const std::wstring& label, float norm01, const std::wstring& valStr,
                             bool showBorderColorDot = false) {
        drawTxt(label, m_captionFmt.Get(), D2D1::RectF(padL, topY, padL + 60.0f, topY + 20.0f), colSecondary);
        float trackL = padL + 64.0f;
        if (showBorderColorDot) {
            const float dcx = padL + 54.0f;
            const float dcy = topY + 10.0f;
            m_hitZones.push_back({Hit_CycleInsetBorderColor, D2D1::RectF(dcx - 9.0f, dcy - 9.0f, dcx + 9.0f, dcy + 9.0f), -1});
            setBrush(hexToD2DColor(m_config.insetBorderColorHex));
            m_renderTarget->FillEllipse(D2D1::Ellipse({dcx, dcy}, 6.5f, 6.5f), brush);
            setBrush(colAccent);
            m_renderTarget->DrawEllipse(D2D1::Ellipse({dcx, dcy}, 7.5f, 7.5f), brush, 1.2f);
            trackL = padL + 68.0f;
        }
        const D2D1_RECT_F trk = D2D1::RectF(trackL, topY + 8.0f, padR - 42.0f, topY + 12.5f);
        m_sliderTrackRects[static_cast<size_t>(kind)] = trk;
        m_hitZones.push_back({hitId, D2D1::RectF(trk.left - 4.0f, topY, trk.right + 4.0f, topY + 20.0f), -1});

        setBrush(colChipBg);
        m_renderTarget->FillRoundedRectangle(D2D1::RoundedRect(trk, 2.2f, 2.2f), brush);
        const float clamped = std::clamp(norm01, 0.0f, 1.0f);
        const float kx = trk.left + (trk.right - trk.left) * clamped;
        setBrush(colAccent);
        m_renderTarget->FillRoundedRectangle(
            D2D1::RoundedRect(D2D1::RectF(trk.left, trk.top, (std::max)(trk.left + 2.0f, kx), trk.bottom), 2.2f, 2.2f), brush);
        m_renderTarget->FillEllipse(D2D1::Ellipse({kx, (trk.top + trk.bottom) * 0.5f}, 5.2f, 5.2f), brush);

        drawTxt(valStr, m_monoFmt.Get(), D2D1::RectF(padR - 40.0f, topY, padR, topY + 20.0f),
                colPrimary, DWRITE_TEXT_ALIGNMENT_TRAILING);
    };

    if (m_activeTab == LeftRailTab::Annotations) {
        renderAnnotationDrawer(padL, padR, curY, drawerRc.bottom - 10.0f, dark, isZh, brush);
        return;
    }

    if (m_activeTab == LeftRailTab::CameraCursor) {
        drawTxt(isZh ? L"鼠标自动运镜" : L"Auto Zoom", m_sectionFmt.Get(),
                D2D1::RectF(padL, curY, padR, curY + 20.0f), colPrimary);
        curY += 24.0f;
        const float w3 = (fullW - 12.0f) / 3.0f;
        const bool zOff = !m_config.autoZoomEnabled || m_config.zoomMode == StudioZoomMode::None;
        const bool z2D = m_config.autoZoomEnabled && m_config.zoomMode == StudioZoomMode::Zoom2D;
        const bool z3D = m_config.autoZoomEnabled && m_config.zoomMode == StudioZoomMode::Motion3D;
        drawOptionChip(Hit_ZoomMode_None, D2D1::RectF(padL, curY, padL + w3, curY + 26.0f), isZh ? L"关闭" : L"Off", zOff);
        drawOptionChip(Hit_ZoomMode_2D, D2D1::RectF(padL + w3 + 6.0f, curY, padL + w3 * 2 + 6.0f, curY + 26.0f), isZh ? L"2D 运镜" : L"2D Zoom", z2D);
        drawOptionChip(Hit_ZoomMode_3D, D2D1::RectF(padL + (w3 + 6.0f) * 2, curY, padR, curY + 26.0f), isZh ? L"3D 运镜" : L"3D Motion", z3D);
        curY += 34.0f;

        drawTxt(isZh ? L"默认放大倍率" : L"Zoom Factor", m_sectionFmt.Get(),
                D2D1::RectF(padL, curY, padR, curY + 20.0f), colPrimary);
        curY += 24.0f;
        drawOptionChip(Hit_ZoomPreset_150, D2D1::RectF(padL, curY, padL + w3, curY + 25.0f),
                       isZh ? L"温和 1.5x" : L"1.5x", std::abs(m_config.defaultZoomFactor - 1.50f) < 0.12f);
        drawOptionChip(Hit_ZoomPreset_185, D2D1::RectF(padL + w3 + 6.0f, curY, padL + w3 * 2 + 6.0f, curY + 25.0f),
                       isZh ? L"标准 1.85x" : L"1.85x", std::abs(m_config.defaultZoomFactor - 1.85f) < 0.15f);
        drawOptionChip(Hit_ZoomPreset_220, D2D1::RectF(padL + (w3 + 6.0f) * 2, curY, padR, curY + 25.0f),
                       isZh ? L"特写 2.2x" : L"2.2x", std::abs(m_config.defaultZoomFactor - 2.20f) < 0.15f);
        curY += 34.0f;

        drawTxt(isZh ? L"推拉运动模糊" : L"Motion Blur", m_bodyFmt.Get(), D2D1::RectF(padL, curY, padR - 44.0f, curY + 20.0f), colPrimary);
        drawTogglePill(Hit_ToggleMotionBlur, padR, curY, m_config.motionBlurEnabled);
        curY += 30.0f;

        setBrush(colBorder);
        m_renderTarget->DrawLine({padL, curY}, {padR, curY}, brush, 1.0f);
        curY += 12.0f;

        drawTxt(isZh ? L"平滑矢量光标" : L"Smooth Vector Cursor", m_sectionFmt.Get(),
                D2D1::RectF(padL, curY, padR - 44.0f, curY + 20.0f), colPrimary);
        drawTogglePill(Hit_ToggleSmoothCursor, padR, curY, m_config.smoothCursorEnabled);
        curY += 26.0f;

        drawOptionChip(Hit_CursorScale_100, D2D1::RectF(padL, curY, padL + w3, curY + 25.0f),
                       L"1.0x", m_config.smoothCursorEnabled && std::abs(m_config.cursorScale - 1.00f) < 0.15f);
        drawOptionChip(Hit_CursorScale_135, D2D1::RectF(padL + w3 + 6.0f, curY, padL + w3 * 2 + 6.0f, curY + 25.0f),
                       L"1.35x", m_config.smoothCursorEnabled && std::abs(m_config.cursorScale - 1.35f) < 0.15f);
        drawOptionChip(Hit_CursorScale_170, D2D1::RectF(padL + (w3 + 6.0f) * 2, curY, padR, curY + 25.0f),
                       L"1.7x", m_config.smoothCursorEnabled && std::abs(m_config.cursorScale - 1.70f) < 0.15f);
        curY += 34.0f;

        drawTxt(isZh ? L"光标聚光环跟随" : L"Cursor Highlight Halo", m_bodyFmt.Get(),
                D2D1::RectF(padL, curY, padR - 44.0f, curY + 20.0f), colPrimary);
        drawTogglePill(Hit_ToggleCursorHalo, padR, curY, m_config.cursorHighlightHalo);
        return;
    }

    if (m_activeTab == LeftRailTab::Captions) {
        drawTxt(isZh ? L"离线语音字幕对齐" : L"Offline Auto Captions", m_sectionFmt.Get(),
                D2D1::RectF(padL, curY, padR - 44.0f, curY + 20.0f), colPrimary);
        drawTogglePill(Hit_ToggleCaptions, padR, curY, m_config.autoCaptionsEnabled);
        curY += 30.0f;
        drawTxt(isZh ? L"开启麦克风录制时，自动识别语音生成底部字幕并导出同名 .srt 文件。"
                     : L"Transcribes mic audio into synced bottom captions and .srt file.",
                m_captionFmt.Get(), D2D1::RectF(padL, curY, padR, curY + 44.0f), colSecondary);
        return;
    }

    drawTxt(isZh ? L"视频尺寸" : L"Video Size", m_sectionFmt.Get(), D2D1::RectF(padL, curY, padR, curY + 19.0f), colPrimary);
    curY += 22.0f;
    {
        static const wchar_t* aspZh[5] = { L"原始", L"16:9", L"9:16", L"4:3", L"1:1" };
        static const wchar_t* aspEn[5] = { L"Orig", L"16:9", L"9:16", L"4:3", L"1:1" };
        const float aw = (fullW - 16.0f) / 5.0f;
        for (int a = 0; a < 5; ++a) {
            const float ax = padL + static_cast<float>(a) * (aw + 4.0f);
            drawOptionChip(Hit_AspectBase + a, D2D1::RectF(ax, curY, ax + aw, curY + 23.0f),
                           isZh ? aspZh[a] : aspEn[a], static_cast<int>(m_config.aspectRatio) == a);
        }
    }
    curY += 29.0f;

    drawTxt(isZh ? L"样式" : L"Style", m_sectionFmt.Get(), D2D1::RectF(padL, curY, padR - 44.0f, curY + 19.0f), colPrimary);
    drawTogglePill(Hit_ToggleAutoFrame, padR, curY, m_config.autoFrameEnabled);
    curY += 23.0f;

    wchar_t vBuf[32]{};
    std::swprintf(vBuf, std::size(vBuf), L"%d%%", static_cast<int>(std::round(m_config.paddingRatio * 100.0f)));
    drawSliderRow(SliderDragKind::Padding, Hit_SliderPadding, curY, isZh ? L"内边距" : L"Padding", m_config.paddingRatio / 0.16f, vBuf);
    curY += 22.5f;

    std::swprintf(vBuf, std::size(vBuf), L"%dpx", static_cast<int>(std::round(m_config.insetBorderWidth)));
    drawSliderRow(SliderDragKind::InsetBorder, Hit_SliderInsetBorder, curY, isZh ? L"内边框" : L"Border", m_config.insetBorderWidth / 12.0f, vBuf, true);
    curY += 22.5f;

    std::swprintf(vBuf, std::size(vBuf), L"%dpx", static_cast<int>(std::round(m_config.cornerRadius)));
    drawSliderRow(SliderDragKind::Radius, Hit_SliderRadius, curY, isZh ? L"圆角" : L"Radius", m_config.cornerRadius / 36.0f, vBuf);
    curY += 22.5f;

    std::swprintf(vBuf, std::size(vBuf), L"%d%%", static_cast<int>(std::round(m_config.shadowIntensity * 100.0f)));
    drawSliderRow(SliderDragKind::Shadow, Hit_SliderShadow, curY, isZh ? L"阴影" : L"Shadow", m_config.shadowIntensity, vBuf);
    curY += 22.5f;

    std::swprintf(vBuf, std::size(vBuf), L"%dpx", static_cast<int>(std::round(m_config.backgroundBlur)));
    drawSliderRow(SliderDragKind::BgBlur, Hit_SliderBgBlur, curY, isZh ? L"背景模糊" : L"BG Blur", m_config.backgroundBlur / 40.0f, vBuf);
    curY += 26.0f;

    setBrush(colBorder);
    m_renderTarget->DrawLine({padL, curY}, {padR, curY}, brush, 1.0f);
    curY += 8.0f;

    const int perPage = static_cast<int>(kWallpaperSlotsPerPage);
    const int totalPages = (std::max)(1, static_cast<int>((static_cast<int>(m_filteredWallpaperIndices.size()) + perPage - 1) / perPage));
    drawTxt(isZh ? L"背景" : L"Background", m_sectionFmt.Get(), D2D1::RectF(padL, curY, padL + 80.0f, curY + 20.0f), colPrimary);

    const D2D1_RECT_F pickRc = D2D1::RectF(padR - (totalPages > 1 ? 132.0f : 56.0f), curY + 1.0f,
                                           padR - (totalPages > 1 ? 78.0f : 0.0f), curY + 20.0f);
    drawOptionChip(Hit_WallPickCustom, pickRc, isZh ? L"+选图" : L"+Img", false);
    if (totalPages > 1) {
        const D2D1_RECT_F prevRc = D2D1::RectF(padR - 72.0f, curY + 1.0f, padR - 53.0f, curY + 20.0f);
        const D2D1_RECT_F nextRc = D2D1::RectF(padR - 19.0f, curY + 1.0f, padR, curY + 20.0f);
        drawOptionChip(Hit_WallPrevPage, prevRc, L"<", false);
        drawOptionChip(Hit_WallNextPage, nextRc, L">", false);
        wchar_t pgTxt[24]{};
        std::swprintf(pgTxt, std::size(pgTxt), L"%d/%d", m_wallpaperPage + 1, totalPages);
        drawTxt(pgTxt, m_monoFmt.Get(), D2D1::RectF(prevRc.right, curY, nextRc.left, curY + 20.0f),
                colSecondary, DWRITE_TEXT_ALIGNMENT_CENTER);
    }
    curY += 24.0f;

    {
        static const wchar_t* catZh[5] = { L"默认", L"沉稳", L"轻盈", L"风景", L"自定义" };
        static const wchar_t* catEn[5] = { L"All", L"Dark", L"Light", L"Scene", L"Custom" };
        const float cw = (fullW - 16.0f) / 5.0f;
        for (int c = 0; c < 5; ++c) {
            const float cxPos = padL + static_cast<float>(c) * (cw + 4.0f);
            drawOptionChip(Hit_WallCatBase + c, D2D1::RectF(cxPos, curY, cxPos + cw, curY + 22.0f),
                           isZh ? catZh[c] : catEn[c], m_wallpaperCategory == c);
        }
    }
    curY += 27.0f;

    {
        const float gap = 5.5f;
        const float boxSize = (fullW - gap * static_cast<float>(kWallpaperCols - 1)) / static_cast<float>(kWallpaperCols);
        m_wallpaperGridRect = D2D1::RectF(padL, curY, padR, curY + boxSize * kWallpaperRows + gap * (kWallpaperRows - 1));

        const size_t startIdx = static_cast<size_t>(m_wallpaperPage) * kWallpaperSlotsPerPage;
        for (size_t slot = 0; slot < kWallpaperSlotsPerPage; ++slot) {
            const size_t fltPos = startIdx + slot;
            const size_t col = slot % kWallpaperCols;
            const size_t row = slot / kWallpaperCols;
            const float tx = padL + static_cast<float>(col) * (boxSize + gap);
            const float ty = curY + static_cast<float>(row) * (boxSize + gap);
            const D2D1_RECT_F trc = D2D1::RectF(tx, ty, tx + boxSize, ty + boxSize);

            if (m_wallpaperCategory == 4 && slot == 0 && m_filteredWallpaperIndices.empty()) {
                m_hitZones.push_back({Hit_WallPickCustom, trc, -1});
                setBrush(colChipBg);
                m_renderTarget->FillRoundedRectangle(D2D1::RoundedRect(trc, 6.0f, 6.0f), brush);
                setBrush(colAccent);
                m_renderTarget->DrawRoundedRectangle(D2D1::RoundedRect(trc, 6.0f, 6.0f), brush, 1.2f);
                drawTxt(L"+", m_titleFmt.Get(), trc, colAccent, DWRITE_TEXT_ALIGNMENT_CENTER);
                break;
            }

            if (fltPos >= m_filteredWallpaperIndices.size()) break;
            const auto& entry = m_allWallpapers[static_cast<size_t>(m_filteredWallpaperIndices[fltPos])];
            const int hId = Hit_ThemeSlotBase + static_cast<int>(slot);
            m_hitZones.push_back({hId, trc, m_filteredWallpaperIndices[fltPos]});

            ComPtr<ID2D1RoundedRectangleGeometry> clipGeo;
            if (SUCCEEDED(m_d2dFactory->CreateRoundedRectangleGeometry(
                    D2D1::RoundedRect(trc, 6.0f, 6.0f), clipGeo.GetAddressOf())) && clipGeo && m_themeThumbBitmaps[slot]) {
                ComPtr<ID2D1Layer> clipLayer;
                if (SUCCEEDED(m_renderTarget->CreateLayer(nullptr, clipLayer.GetAddressOf())) && clipLayer) {
                    m_renderTarget->PushLayer(D2D1::LayerParameters(trc, clipGeo.Get()), clipLayer.Get());
                    m_renderTarget->DrawBitmap(m_themeThumbBitmaps[slot].Get(), trc, 1.0f, D2D1_BITMAP_INTERPOLATION_MODE_LINEAR);
                    m_renderTarget->PopLayer();
                }
            }
            const bool selected = entry.isBuiltin
                ? (m_config.customBackgroundPath.empty() && m_config.backgroundTheme == entry.builtinTheme)
                : (!m_config.customBackgroundPath.empty() && m_config.customBackgroundPath == entry.filePathUtf8);
            if (selected) {
                setBrush(D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.92f));
                m_renderTarget->DrawRoundedRectangle(D2D1::RoundedRect(trc, 6.0f, 6.0f), brush, 2.5f);
                setBrush(colAccent);
                const D2D1_RECT_F outRc = D2D1::RectF(trc.left - 1.2f, trc.top - 1.2f, trc.right + 1.2f, trc.bottom + 1.2f);
                m_renderTarget->DrawRoundedRectangle(D2D1::RoundedRect(outRc, 7.0f, 7.0f), brush, 1.6f);
            } else {
                setBrush(m_hoveredHitId == hId ? colAccent : colBorder);
                m_renderTarget->DrawRoundedRectangle(D2D1::RoundedRect(trc, 6.0f, 6.0f), brush, m_hoveredHitId == hId ? 1.5f : 0.9f);
            }
        }
    }
}

void StudioPreviewWindow::renderExportModalOverlay(float W, float H, bool dark, bool isZh, ID2D1SolidColorBrush* brush) {
    m_hitZones.clear();

    const auto accentRgb = tools3000::core::NativeThemePalette::getEffectiveAccentColorRGB();
    const D2D1_COLOR_F colAccent = D2D1::ColorF(accentRgb.r, accentRgb.g, accentRgb.b, 1.0f);
    const D2D1_COLOR_F colModalBg = dark
        ? D2D1::ColorF(0.082f, 0.094f, 0.133f, 0.985f)
        : D2D1::ColorF(0.992f, 0.995f, 1.000f, 0.990f);
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

    const D2D1_RECT_F fullRc = D2D1::RectF(0.0f, 0.0f, W, H);
    if (!exporting) {
        m_hitZones.push_back({Hit_ModalBackdrop, fullRc, -1});
    }
    setBrush(D2D1::ColorF(0.02f, 0.03f, 0.06f, dark ? 0.68f : 0.45f));
    m_renderTarget->FillRectangle(fullRc, brush);

    const float cardW = 508.0f;
    const float cardH = 448.0f;
    const float cardL = (W - cardW) * 0.5f;
    const float cardT = (H - cardH) * 0.5f;
    const D2D1_RECT_F cardRc = D2D1::RectF(cardL, cardT, cardL + cardW, cardT + cardH);
    m_hitZones.push_back({Hit_ModalCardBody, cardRc, -1});

    setBrush(colModalBg);
    m_renderTarget->FillRoundedRectangle(D2D1::RoundedRect(cardRc, 16.0f, 16.0f), brush);
    setBrush(colBorder);
    m_renderTarget->DrawRoundedRectangle(D2D1::RoundedRect(cardRc, 16.0f, 16.0f), brush, 1.2f);

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
        Hit_ModalFormatSelect, inL, y, isZh ? L"格式" : L"Format",
        formatLabel(m_exportOpts.format, isZh), m_openDropdown == ExportDropdownKind::Format);
    const D2D1_RECT_F resBox = drawSelectBox(
        Hit_ModalResSelect, rightColL, y, isZh ? L"分辨率" : L"Resolution",
        resolutionLabel(m_exportOpts.resolution, isZh), m_openDropdown == ExportDropdownKind::Resolution);

    y += 72.0f;
    wchar_t fpsTxt[32]{};
    std::swprintf(fpsTxt, std::size(fpsTxt), L"%d FPS", m_exportOpts.fps);
    const D2D1_RECT_F fpsBox = drawSelectBox(
        Hit_ModalFpsSelect, inL, y, isZh ? L"帧率" : L"Frame Rate",
        fpsTxt, m_openDropdown == ExportDropdownKind::Fps);
    const D2D1_RECT_F qualBox = drawSelectBox(
        Hit_ModalQualitySelect, rightColL, y, isZh ? L"质量" : L"Quality",
        qualityLabel(m_exportOpts.quality, isZh), m_openDropdown == ExportDropdownKind::Quality);

    y += 68.0f;
    {
        std::wstring fmtTip;
        if (m_exportOpts.format == StudioExportFormat::GIF_WeChat) {
            fmtTip = isZh
                ? L"提示：<=800px 微信按「动态表情」聊天内免点直播；如需在微信双击打开放大请选「GIF 高清大图」或 MP4"
                : L"Note: <=800px auto-plays inline in WeChat as Sticker; choose 'GIF HD' or MP4 for double-click zoom";
        } else if (m_exportOpts.format == StudioExportFormat::GIF_HD) {
            fmtTip = isZh
                ? L"提示：长边 >=1080px 发微信按「普通高清图片」展示，支持双击打开独立图片查看器滚轮放大看清细节"
                : L"Note: >=1080px sends as standard HD image in WeChat — supports double-click viewer zoom";
        } else if (m_exportOpts.format == StudioExportFormat::MP4_H264 ||
                   m_exportOpts.format == StudioExportFormat::MP4_HEVC) {
            fmtTip = isZh
                ? L"提示：完整保留原声音轨与 24-bit 真彩画质，微信/飞书/网页均支持独立大窗高清全屏放大播放"
                : L"Note: Preserves original stereo audio & 24-bit crisp color with full-screen zoom playback";
        } else {
            fmtTip = isZh
                ? L"提示：现代网页真彩无损级动图/视频容器，适合嵌入文档、GitHub 与技术博客"
                : L"Note: True-color web media container ideal for docs, GitHub & blogs";
        }
        drawTxt(fmtTip, m_captionFmt.Get(), D2D1::RectF(inL, y, inR, y + 24.0f), colSecondary);
    }

    y += 32.0f;
    const D2D1_RECT_F chkHit = D2D1::RectF(inL, y, inL + 150.0f, y + 36.0f);
    if (!exporting) {
        m_hitZones.push_back({Hit_ModalSavePrefsCheck, chkHit, -1});
    }
    const D2D1_RECT_F chkBox = D2D1::RectF(inL, y + 9.0f, inL + 18.0f, y + 27.0f);
    if (m_saveExportPrefs) {
        setBrush(colAccent);
        m_renderTarget->FillRoundedRectangle(D2D1::RoundedRect(chkBox, 4.5f, 4.5f), brush);
        setBrush(D2D1::ColorF(1.0f, 1.0f, 1.0f, 1.0f));
        m_renderTarget->DrawLine({chkBox.left + 4.0f, chkBox.top + 9.2f}, {chkBox.left + 7.8f, chkBox.top + 13.0f}, brush, 1.9f);
        m_renderTarget->DrawLine({chkBox.left + 7.8f, chkBox.top + 13.0f}, {chkBox.left + 14.2f, chkBox.top + 5.2f}, brush, 1.9f);
    } else {
        setBrush(colFieldBg);
        m_renderTarget->FillRoundedRectangle(D2D1::RoundedRect(chkBox, 4.5f, 4.5f), brush);
        setBrush(colBorder);
        m_renderTarget->DrawRoundedRectangle(D2D1::RoundedRect(chkBox, 4.5f, 4.5f), brush, 1.2f);
    }
    drawTxt(isZh ? L"保存当前设置" : L"Save Settings", m_bodyFmt.Get(),
            D2D1::RectF(chkBox.right + 8.0f, y, chkHit.right, y + 36.0f), colSecondary);

    const cv::Size targetCanvasSz = StudioVideoCompositor::resolveOutputCanvasSize(
        m_origWidth, m_origHeight, m_config.aspectRatio);
    const uint64_t estBytes = StudioVideoCompositor::estimateExportSizeBytes(
        targetCanvasSz.width, targetCanvasSz.height, computeEffectiveDurationSec(), m_exportOpts);
    const std::wstring estStr = formatEstimatedSize(estBytes);
    const D2D1_RECT_F expBtn = D2D1::RectF(inR - 206.0f, y - 1.0f, inR, y + 37.0f);
    if (!exporting) {
        m_hitZones.push_back({Hit_ModalConfirmExportBtn, expBtn, -1});
    }

    if (exporting) {
        const float prog = static_cast<float>(std::clamp(m_exportProgress.load(std::memory_order_relaxed), 0.0, 1.0));
        setBrush(colFieldBg);
        m_renderTarget->FillRoundedRectangle(D2D1::RoundedRect(expBtn, 8.0f, 8.0f), brush);
        const D2D1_RECT_F fillRc = D2D1::RectF(
            expBtn.left, expBtn.top,
            expBtn.left + (std::max)(10.0f, (expBtn.right - expBtn.left) * prog), expBtn.bottom);
        setBrush(colAccent);
        m_renderTarget->FillRoundedRectangle(D2D1::RoundedRect(fillRc, 8.0f, 8.0f), brush);
        wchar_t pTxt[64]{};
        std::swprintf(pTxt, std::size(pTxt), isZh ? L"正在合成 %d%%..." : L"Exporting %d%%...",
                      static_cast<int>(std::round(prog * 100.0f)));
        drawTxt(pTxt, m_sectionFmt.Get(), expBtn, D2D1::ColorF(1.0f, 1.0f, 1.0f, 1.0f), DWRITE_TEXT_ALIGNMENT_CENTER);
    } else {
        setBrush(m_hoveredHitId == Hit_ModalConfirmExportBtn
            ? D2D1::ColorF((std::min)(1.0f, accentRgb.r + 0.07f), (std::min)(1.0f, accentRgb.g + 0.07f), (std::min)(1.0f, accentRgb.b + 0.07f), 1.0f)
            : colAccent);
        m_renderTarget->FillRoundedRectangle(D2D1::RoundedRect(expBtn, 8.0f, 8.0f), brush);
        const std::wstring btnTxt = (isZh ? L"导出  (~" : L"Export (~") + estStr + L")";
        drawTxt(btnTxt, m_sectionFmt.Get(), expBtn, D2D1::ColorF(1.0f, 1.0f, 1.0f, 1.0f), DWRITE_TEXT_ALIGNMENT_CENTER);
    }

    if (!exporting && m_openDropdown != ExportDropdownKind::None) {
        struct DropItem { int payload; std::wstring text; bool selected; };
        std::vector<DropItem> items;
        D2D1_RECT_F anchor{};

        if (m_openDropdown == ExportDropdownKind::Format) {
            anchor = fmtBox;
            const StudioExportFormat fmts[] = {
                StudioExportFormat::MP4_H264, StudioExportFormat::MP4_HEVC,
                StudioExportFormat::GIF_WeChat, StudioExportFormat::GIF_HD,
                StudioExportFormat::WebP_Animated, StudioExportFormat::WebM_VP9
            };
            for (auto f : fmts) {
                items.push_back({static_cast<int>(f), formatLabel(f, isZh), m_exportOpts.format == f});
            }
        } else if (m_openDropdown == ExportDropdownKind::Resolution) {
            anchor = resBox;
            const StudioExportResolution resList[] = {
                StudioExportResolution::Original, StudioExportResolution::Res2160P,
                StudioExportResolution::Res1440P, StudioExportResolution::Res1080P,
                StudioExportResolution::Res720P,  StudioExportResolution::Res640P,
                StudioExportResolution::Res480P
            };
            for (auto r : resList) {
                items.push_back({static_cast<int>(r), resolutionLabel(r, isZh), m_exportOpts.resolution == r});
            }
        } else if (m_openDropdown == ExportDropdownKind::Fps) {
            anchor = fpsBox;
            const bool isGif = (m_exportOpts.format == StudioExportFormat::GIF_WeChat ||
                                m_exportOpts.format == StudioExportFormat::GIF_HD);
            if (isGif) {
                for (int f : {10, 12, 15}) {
                    items.push_back({f, std::to_wstring(f) + L" FPS", m_exportOpts.fps == f});
                }
            } else {
                for (int f : {24, 30, 60}) {
                    items.push_back({f, std::to_wstring(f) + L" FPS", m_exportOpts.fps == f});
                }
            }
        } else if (m_openDropdown == ExportDropdownKind::Quality) {
            anchor = qualBox;
            for (auto q : {StudioExportQuality::High, StudioExportQuality::Recommended, StudioExportQuality::Compact}) {
                items.push_back({static_cast<int>(q), qualityLabel(q, isZh), m_exportOpts.quality == q});
            }
        }

        if (!items.empty()) {
            const float rowH = 32.0f;
            const float menuH = static_cast<float>(items.size()) * rowH + 8.0f;
            float menuTop = anchor.bottom + 4.0f;
            if (menuTop + menuH > H - 10.0f) {
                menuTop = (std::max)(10.0f, anchor.top - menuH - 4.0f);
            }
            const D2D1_RECT_F menuRc = D2D1::RectF(anchor.left, menuTop, anchor.right, menuTop + menuH);
            setBrush(dark ? D2D1::ColorF(0.10f, 0.12f, 0.17f, 0.99f) : D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.99f));
            m_renderTarget->FillRoundedRectangle(D2D1::RoundedRect(menuRc, 8.0f, 8.0f), brush);
            setBrush(colAccent);
            m_renderTarget->DrawRoundedRectangle(D2D1::RoundedRect(menuRc, 8.0f, 8.0f), brush, 1.2f);

            for (size_t i = 0; i < items.size(); ++i) {
                const float iy = menuTop + 4.0f + static_cast<float>(i) * rowH;
                const D2D1_RECT_F itemRc = D2D1::RectF(menuRc.left + 4.0f, iy, menuRc.right - 4.0f, iy + rowH);
                const int itemHitId = Hit_ModalDropdownItemBase + static_cast<int>(i);
                m_hitZones.push_back({itemHitId, itemRc, items[i].payload});
                if (items[i].selected || m_hoveredHitId == itemHitId) {
                    setBrush(items[i].selected
                        ? D2D1::ColorF(accentRgb.r, accentRgb.g, accentRgb.b, dark ? 0.28f : 0.16f)
                        : (dark ? D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.08f) : D2D1::ColorF(0.06f, 0.09f, 0.16f, 0.06f)));
                    m_renderTarget->FillRoundedRectangle(D2D1::RoundedRect(itemRc, 5.0f, 5.0f), brush);
                }
                drawTxt(items[i].text, m_bodyFmt.Get(),
                        D2D1::RectF(itemRc.left + 10.0f, itemRc.top, itemRc.right - 8.0f, itemRc.bottom),
                        items[i].selected ? colAccent : colPrimary);
            }
        }
    }
}

void StudioPreviewWindow::render() {
    if (!ensureRenderResources()) return;

    const D2D1_SIZE_F sz = m_renderTarget->GetSize();
    const float W = sz.width;
    const float H = sz.height;
    const bool dark = tools3000::core::NativeThemePalette::isEffectiveDarkMode();
    const bool isZh = tools3000::core::NativeThemePalette::isUiLanguageChinese();
    const auto accentRgb = tools3000::core::NativeThemePalette::getEffectiveAccentColorRGB();

    m_hitZones.clear();
    m_renderTarget->BeginDraw();

    const D2D1_COLOR_F colWindowBg = dark
        ? D2D1::ColorF(0.055f, 0.070f, 0.105f, 1.0f)
        : D2D1::ColorF(0.969f, 0.976f, 0.988f, 1.0f);
    const D2D1_COLOR_F colCardBg = dark
        ? D2D1::ColorF(0.092f, 0.114f, 0.165f, 1.0f)
        : D2D1::ColorF(1.000f, 1.000f, 1.000f, 1.0f);
    const D2D1_COLOR_F colStageBg = dark
        ? D2D1::ColorF(0.036f, 0.044f, 0.068f, 1.0f)
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
    const D2D1_COLOR_F colAccentSoft = D2D1::ColorF(accentRgb.r, accentRgb.g, accentRgb.b, dark ? 0.24f : 0.14f);
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
                               DWRITE_TEXT_ALIGNMENT hAlign = DWRITE_TEXT_ALIGNMENT_LEADING) {
        if (!fmt || txt.empty()) return;
        fmt->SetTextAlignment(hAlign);
        fmt->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
        setBrush(col);
        m_renderTarget->DrawText(txt.c_str(), static_cast<UINT32>(txt.size()), fmt, r, brush.Get());
    };

    // 1. 顶部标题栏 + 右上角 [保留原片] / [导出] 按钮
    const float headerH = 44.0f;
    const std::wstring headerTitle = isZh ? L"Tools3000 录屏演播室" : L"Tools3000 Studio";
    drawTextAligned(headerTitle, m_titleFmt.Get(), D2D1::RectF(16.0f, 0.0f, 240.0f, headerH), colPrimary);

    const double keptDurSec = computeEffectiveDurationSec();
    wchar_t metaBuf[180]{};
    if (std::abs(keptDurSec - m_durationSec) > 0.08) {
        std::swprintf(metaBuf, std::size(metaBuf),
                      isZh ? L"%dx%d · 原长 %.1fs -> 成片 %.1fs · S 拆分切片 · Del 删除选中 · Ctrl+Z 撤销"
                           : L"%dx%d · %.1fs -> %.1fs · S Split · Del Remove · Ctrl+Z Undo",
                      m_origWidth, m_origHeight, m_durationSec, keptDurSec);
    } else {
        std::swprintf(metaBuf, std::size(metaBuf),
                      isZh ? L"%dx%d · %.1fs · 点轨道定位置 -> [拆分(S)] 切两刀 -> 点中间切片按 Del 删除"
                           : L"%dx%d · %.1fs · Click track -> [Split(S)] twice -> Select middle clip & press Del",
                      m_origWidth, m_origHeight, m_durationSec);
    }
    drawTextAligned(metaBuf, m_captionFmt.Get(), D2D1::RectF(216.0f, 0.0f, W - 252.0f, headerH), colSecondary);

    const bool exporting = m_exporting.load(std::memory_order_acquire);
    const D2D1_RECT_F closeRc = D2D1::RectF(W - 40.0f, 8.0f, W - 12.0f, 36.0f);
    const D2D1_RECT_F exportBtnRc = D2D1::RectF(closeRc.left - 96.0f, 8.0f, closeRc.left - 8.0f, 36.0f);
    const D2D1_RECT_F keepBtnRc = D2D1::RectF(exportBtnRc.left - 94.0f, 8.0f, exportBtnRc.left - 8.0f, 36.0f);

    if (!exporting && !m_exportModalOpen) {
        m_hitZones.push_back({Hit_KeepOriginalBtn, keepBtnRc, -1});
        m_hitZones.push_back({Hit_OpenExportModalBtn, exportBtnRc, -1});
    }
    m_hitZones.push_back({Hit_CloseBtn, closeRc, -1});

    setBrush(m_hoveredHitId == Hit_KeepOriginalBtn ? colAccentSoft : colChipBg);
    m_renderTarget->FillRoundedRectangle(D2D1::RoundedRect(keepBtnRc, 7.0f, 7.0f), brush.Get());
    setBrush(colBorder);
    m_renderTarget->DrawRoundedRectangle(D2D1::RoundedRect(keepBtnRc, 7.0f, 7.0f), brush.Get(), 1.0f);
    drawTextAligned(isZh ? L"保留原片" : L"Keep Raw", m_captionFmt.Get(), keepBtnRc, colPrimary, DWRITE_TEXT_ALIGNMENT_CENTER);

    setBrush(m_hoveredHitId == Hit_OpenExportModalBtn
        ? D2D1::ColorF((std::min)(1.0f, accentRgb.r + 0.07f), (std::min)(1.0f, accentRgb.g + 0.07f), (std::min)(1.0f, accentRgb.b + 0.07f), 1.0f)
        : colAccent);
    m_renderTarget->FillRoundedRectangle(D2D1::RoundedRect(exportBtnRc, 7.0f, 7.0f), brush.Get());
    drawTextAligned(isZh ? L"导出" : L"Export", m_sectionFmt.Get(), exportBtnRc,
                    D2D1::ColorF(1.0f, 1.0f, 1.0f, 1.0f), DWRITE_TEXT_ALIGNMENT_CENTER);

    if (m_hoveredHitId == Hit_CloseBtn && !m_exportModalOpen) {
        setBrush(D2D1::ColorF(0.937f, 0.267f, 0.267f, 0.18f));
        m_renderTarget->FillRoundedRectangle(D2D1::RoundedRect(closeRc, 7.0f, 7.0f), brush.Get());
    }
    setBrush((m_hoveredHitId == Hit_CloseBtn && !m_exportModalOpen) ? D2D1::ColorF(0.937f, 0.267f, 0.267f, 1.0f) : colSecondary);
    const float cx = (closeRc.left + closeRc.right) * 0.5f;
    const float cy = (closeRc.top + closeRc.bottom) * 0.5f;
    m_renderTarget->DrawLine({cx - 5.0f, cy - 5.0f}, {cx + 5.0f, cy + 5.0f}, brush.Get(), 1.8f);
    m_renderTarget->DrawLine({cx + 5.0f, cy - 5.0f}, {cx - 5.0f, cy + 5.0f}, brush.Get(), 1.8f);

    setBrush(colBorder);
    m_renderTarget->DrawLine({0.0f, headerH}, {W, headerH}, brush.Get(), 1.0f);

    // 2. 左侧图标栏 + 左侧属性/标注抽屉面板
    const float contentTop = headerH + 10.0f;
    const float contentBottom = H - 12.0f;
    const D2D1_RECT_F railRc = D2D1::RectF(12.0f, contentTop, 58.0f, contentBottom);
    const D2D1_RECT_F drawerRc = D2D1::RectF(railRc.right + 8.0f, contentTop, railRc.right + 294.0f, contentBottom);
    renderLeftRailAndDrawer(railRc, drawerRc, dark, isZh, brush.Get());

    // 3. 右侧宽敞预览大舞台
    const float rightAreaLeft = drawerRc.right + 10.0f;
    const float rightAreaRight = W - 12.0f;
    const float timelineCardH = 180.0f;
    const float stageBottom = contentBottom - timelineCardH - 8.0f;
    const D2D1_RECT_F stageRc = D2D1::RectF(rightAreaLeft, contentTop, rightAreaRight, stageBottom);

    setBrush(colStageBg);
    m_renderTarget->FillRoundedRectangle(D2D1::RoundedRect(stageRc, 12.0f, 12.0f), brush.Get());
    setBrush(colBorder);
    m_renderTarget->DrawRoundedRectangle(D2D1::RoundedRect(stageRc, 12.0f, 12.0f), brush.Get(), 1.0f);

    const D2D1_RECT_F videoViewport = D2D1::RectF(
        stageRc.left + 10.0f, stageRc.top + 10.0f, stageRc.right - 10.0f, stageRc.bottom - 10.0f);
    m_hitZones.push_back({Hit_StageCanvas, videoViewport, -1});

    if (m_frameBitmap && m_bitmapW > 0 && m_bitmapH > 0) {
        const float availW = (std::max)(32.0f, videoViewport.right - videoViewport.left);
        const float availH = (std::max)(32.0f, videoViewport.bottom - videoViewport.top);
        const float fit = (std::min)(availW / static_cast<float>(m_bitmapW), availH / static_cast<float>(m_bitmapH));
        const float drawW = static_cast<float>(m_bitmapW) * fit;
        const float drawH = static_cast<float>(m_bitmapH) * fit;
        const float dx = videoViewport.left + (availW - drawW) * 0.5f;
        const float dy = videoViewport.top + (availH - drawH) * 0.5f;
        m_lastBitmapDrawRect = D2D1::RectF(dx, dy, dx + drawW, dy + drawH);
        m_renderTarget->DrawBitmap(m_frameBitmap.Get(), m_lastBitmapDrawRect, 1.0f, D2D1_BITMAP_INTERPOLATION_MODE_LINEAR);

        if (m_selectedZoomIdx >= 0 && m_selectedZoomIdx < static_cast<int>(m_timeline.zoomSegments.size())) {
            const auto& zseg = m_timeline.zoomSegments[static_cast<size_t>(m_selectedZoomIdx)];
            const float srcW = static_cast<float>((std::max)(1, m_timeline.sourceWidth));
            const float srcH = static_cast<float>((std::max)(1, m_timeline.sourceHeight));
            float sx = 0.0f, sy = 0.0f;
            if (mapSourceNormToStagePoint(zseg.focusX / srcW, zseg.focusY / srcH, sx, sy) &&
                pointInRectF(m_lastBitmapDrawRect, sx, sy)) {
                setBrush(D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.92f));
                m_renderTarget->DrawEllipse(D2D1::Ellipse({sx, sy}, 9.5f, 9.5f), brush.Get(), 2.2f);
                setBrush(colAccent);
                m_renderTarget->DrawEllipse(D2D1::Ellipse({sx, sy}, 9.5f, 9.5f), brush.Get(), 1.4f);
                m_renderTarget->DrawLine({sx - 14.0f, sy}, {sx + 14.0f, sy}, brush.Get(), 1.2f);
                m_renderTarget->DrawLine({sx, sy - 14.0f}, {sx, sy + 14.0f}, brush.Get(), 1.2f);
            }
        }

        if (m_selectedAnnIdx >= 0 && m_selectedAnnIdx < static_cast<int>(m_timeline.annotations.size())) {
            const auto& ann = m_timeline.annotations[static_cast<size_t>(m_selectedAnnIdx)];
            float p0x = 0.0f, p0y = 0.0f, p1x = 0.0f, p1y = 0.0f;
            if (mapSourceNormToStagePoint(ann.x0, ann.y0, p0x, p0y) &&
                mapSourceNormToStagePoint(ann.x1, ann.y1, p1x, p1y)) {
                setBrush(colAccent);
                if (isPointDirectedAnnotation(ann.kind)) {
                    m_renderTarget->FillEllipse(D2D1::Ellipse({p0x, p0y}, 4.5f, 4.5f), brush.Get());
                    m_renderTarget->FillEllipse(D2D1::Ellipse({p1x, p1y}, 6.0f, 6.0f), brush.Get());
                    setBrush(D2D1::ColorF(1, 1, 1, 0.95f));
                    m_renderTarget->DrawEllipse(D2D1::Ellipse({p1x, p1y}, 6.0f, 6.0f), brush.Get(), 1.6f);
                } else {
                    const D2D1_RECT_F boxRc = D2D1::RectF(
                        (std::min)(p0x, p1x) - 4.0f, (std::min)(p0y, p1y) - 4.0f,
                        (std::max)(p0x, p1x) + 4.0f, (std::max)(p0y, p1y) + 4.0f);
                    D2D1_STROKE_STYLE_PROPERTIES sp = D2D1::StrokeStyleProperties();
                    sp.dashStyle = D2D1_DASH_STYLE_DASH;
                    ComPtr<ID2D1StrokeStyle> ds;
                    m_d2dFactory->CreateStrokeStyle(sp, nullptr, 0, ds.GetAddressOf());
                    m_renderTarget->DrawRoundedRectangle(D2D1::RoundedRect(boxRc, 4.0f, 4.0f), brush.Get(), 1.3f, ds.Get());
                    if (!isTextLikeAnnotation(ann.kind)) {
                        m_renderTarget->FillEllipse(D2D1::Ellipse({boxRc.right, boxRc.bottom}, 5.5f, 5.5f), brush.Get());
                        setBrush(D2D1::ColorF(1, 1, 1, 0.95f));
                        m_renderTarget->DrawEllipse(D2D1::Ellipse({boxRc.right, boxRc.bottom}, 5.5f, 5.5f), brush.Get(), 1.4f);
                    }
                }
            }
        }
    }

    // 4. 右下角磁性波纹切片与三轨非线性时间轴卡片 (154px)
    const D2D1_RECT_F tlCard = D2D1::RectF(rightAreaLeft, stageBottom + 8.0f, rightAreaRight, contentBottom);
    setBrush(colCardBg);
    m_renderTarget->FillRoundedRectangle(D2D1::RoundedRect(tlCard, 12.0f, 12.0f), brush.Get());
    setBrush(colBorder);
    m_renderTarget->DrawRoundedRectangle(D2D1::RoundedRect(tlCard, 12.0f, 12.0f), brush.Get(), 1.0f);

    auto drawMiniActionBtn = [&](int hitId, const D2D1_RECT_F& rc, const std::wstring& label,
                                 const D2D1_COLOR_F& tint, bool enabled = true, bool withScissors = false) {
        if (enabled) {
            m_hitZones.push_back({hitId, rc, -1});
        }
        const bool hov = enabled && (m_hoveredHitId == hitId);
        const float alphaMul = enabled ? 1.0f : 0.35f;
        setBrush(D2D1::ColorF(tint.r, tint.g, tint.b, (hov ? 0.26f : 0.13f) * alphaMul));
        m_renderTarget->FillRoundedRectangle(D2D1::RoundedRect(rc, 5.0f, 5.0f), brush.Get());
        setBrush(D2D1::ColorF(tint.r, tint.g, tint.b, (hov ? 0.85f : 0.45f) * alphaMul));
        m_renderTarget->DrawRoundedRectangle(D2D1::RoundedRect(rc, 5.0f, 5.0f), brush.Get(), 1.0f);

        const D2D1_COLOR_F fgCol = D2D1::ColorF(tint.r, tint.g, tint.b, alphaMul);
        if (withScissors) {
            setBrush(fgCol);
            drawScissorsIcon(m_renderTarget.Get(), brush.Get(), rc.left + 13.0f, (rc.top + rc.bottom) * 0.5f);
            drawTextAligned(label, m_captionFmt.Get(),
                            D2D1::RectF(rc.left + 21.0f, rc.top, rc.right - 4.0f, rc.bottom),
                            fgCol, DWRITE_TEXT_ALIGNMENT_CENTER);
        } else {
            drawTextAligned(label, m_captionFmt.Get(), rc, fgCol, DWRITE_TEXT_ALIGNMENT_CENTER);
        }
    };

    // 时间轴顶栏左侧：[✂ 拆分(S)] | [删除片段] | [倍速] | [撤销] | [重做] | [标注] | [+运镜]
    float leftBtnX = tlCard.left + 10.0f;
    const D2D1_RECT_F splitBtnRc = D2D1::RectF(leftBtnX, tlCard.top + 8.0f, leftBtnX + 74.0f, tlCard.top + 32.0f);
    drawMiniActionBtn(Hit_SplitAtPlayhead, splitBtnRc, isZh ? L"拆分(S)" : L"Split(S)",
                      D2D1::ColorF(0.55f, 0.36f, 0.98f, 1.0f), true, true);
    leftBtnX = splitBtnRc.right + 5.0f;

    const bool canDeleteClip = (m_selectedClipIdx >= 0 && m_videoClips.size() > 1);
    const bool canDeleteAny = canDeleteClip || m_selectedZoomIdx >= 0 || m_selectedAnnIdx >= 0;
    const D2D1_RECT_F delBtnRc = D2D1::RectF(leftBtnX, tlCard.top + 8.0f, leftBtnX + 62.0f, tlCard.top + 32.0f);
    drawMiniActionBtn(Hit_DeleteSelectedItem, delBtnRc, isZh ? L"删除片段" : L"Delete",
                      D2D1::ColorF(0.95f, 0.32f, 0.38f, 1.0f), canDeleteAny, false);
    leftBtnX = delBtnRc.right + 5.0f;

    float curClipSpd = 1.0f;
    if (m_selectedClipIdx >= 0 && m_selectedClipIdx < static_cast<int>(m_videoClips.size())) {
        curClipSpd = m_videoClips[static_cast<size_t>(m_selectedClipIdx)].speed;
    }
    wchar_t spdLabel[24]{};
    std::swprintf(spdLabel, std::size(spdLabel), L"%.1fx", curClipSpd);
    const D2D1_RECT_F spdRc = D2D1::RectF(leftBtnX, tlCard.top + 8.0f, leftBtnX + 42.0f, tlCard.top + 32.0f);
    drawMiniActionBtn(Hit_CycleClipSpeed, spdRc, spdLabel, D2D1::ColorF(0.08f, 0.72f, 0.65f, 1.0f), !m_videoClips.empty());
    leftBtnX = spdRc.right + 5.0f;

    const D2D1_RECT_F undoRc = D2D1::RectF(leftBtnX, tlCard.top + 8.0f, leftBtnX + 40.0f, tlCard.top + 32.0f);
    drawMiniActionBtn(Hit_UndoBtn, undoRc, isZh ? L"撤销" : L"Undo", colSecondary, !m_undoStack.empty());
    leftBtnX = undoRc.right + 4.0f;

    const D2D1_RECT_F redoRc = D2D1::RectF(leftBtnX, tlCard.top + 8.0f, leftBtnX + 40.0f, tlCard.top + 32.0f);
    drawMiniActionBtn(Hit_RedoBtn, redoRc, isZh ? L"重做" : L"Redo", colSecondary, !m_redoStack.empty());
    leftBtnX = redoRc.right + 5.0f;

    const D2D1_RECT_F markQuickRc = D2D1::RectF(leftBtnX, tlCard.top + 8.0f, leftBtnX + 44.0f, tlCard.top + 32.0f);
    drawMiniActionBtn(Hit_QuickOpenAnnotations, markQuickRc, isZh ? L"标注" : L"Mark", D2D1::ColorF(0.96f, 0.58f, 0.12f, 1.0f));
    leftBtnX = markQuickRc.right + 5.0f;

    const D2D1_RECT_F addZoomRc = D2D1::RectF(leftBtnX, tlCard.top + 8.0f, leftBtnX + 50.0f, tlCard.top + 32.0f);
    drawMiniActionBtn(Hit_AddZoomAtPlayhead, addZoomRc, isZh ? L"+运镜" : L"+Zoom", colAccent);
    leftBtnX = addZoomRc.right + 6.0f;

    // 播放按钮 + 时间码 + 顶部进度条
    const D2D1_RECT_F playBtn = D2D1::RectF(leftBtnX + 2.0f, tlCard.top + 7.0f, leftBtnX + 28.0f, tlCard.top + 33.0f);
    m_hitZones.push_back({Hit_PlayPauseBtn, playBtn, -1});
    setBrush(m_hoveredHitId == Hit_PlayPauseBtn ? colAccent : colAccentSoft);
    m_renderTarget->FillRoundedRectangle(D2D1::RoundedRect(playBtn, 6.5f, 6.5f), brush.Get());
    setBrush((m_hoveredHitId == Hit_PlayPauseBtn) ? D2D1::ColorF(1.0f, 1.0f, 1.0f, 1.0f) : colAccent);
    const float pcx = (playBtn.left + playBtn.right) * 0.5f;
    const float pcy = (playBtn.top + playBtn.bottom) * 0.5f;
    if (m_playing) {
        m_renderTarget->FillRoundedRectangle(
            D2D1::RoundedRect(D2D1::RectF(pcx - 4.2f, pcy - 4.8f, pcx - 1.3f, pcy + 4.8f), 1.0f, 1.0f), brush.Get());
        m_renderTarget->FillRoundedRectangle(
            D2D1::RoundedRect(D2D1::RectF(pcx + 1.3f, pcy - 4.8f, pcx + 4.2f, pcy + 4.8f), 1.0f, 1.0f), brush.Get());
    } else {
        ComPtr<ID2D1PathGeometry> tri;
        if (SUCCEEDED(m_d2dFactory->CreatePathGeometry(tri.GetAddressOf())) && tri) {
            ComPtr<ID2D1GeometrySink> sink;
            if (SUCCEEDED(tri->Open(sink.GetAddressOf())) && sink) {
                sink->BeginFigure({pcx - 3.2f, pcy - 5.2f}, D2D1_FIGURE_BEGIN_FILLED);
                sink->AddLine({pcx + 4.8f, pcy});
                sink->AddLine({pcx - 3.2f, pcy + 5.2f});
                sink->EndFigure(D2D1_FIGURE_END_CLOSED);
                sink->Close();
                m_renderTarget->FillGeometry(tri.Get(), brush.Get());
            }
        }
    }

    const double shownKeptSec = m_hoverSkimming ? m_hoverSkimSec : m_playheadSec;
    const std::wstring timeStr = formatTimecodeMmSs(shownKeptSec) + L" / " + formatTimecodeMmSs(keptDurSec);
    drawTextAligned(timeStr, m_monoFmt.Get(),
                    D2D1::RectF(playBtn.right + 6.0f, tlCard.top + 7.0f, playBtn.right + 124.0f, tlCard.top + 33.0f), colPrimary);

    const float laneLeft = tlCard.left + 68.0f;
    const float laneRight = tlCard.right - 14.0f;
    m_timelineTrackRect = D2D1::RectF((std::min)(laneRight - 36.0f, playBtn.right + 126.0f), tlCard.top + 18.0f, laneRight, tlCard.top + 23.0f);
    m_hitZones.push_back({Hit_TimelineTopBarScrub, D2D1::RectF(m_timelineTrackRect.left, tlCard.top + 7.0f, laneRight, tlCard.top + 33.0f), -1});

    setBrush(colChipBg);
    m_renderTarget->FillRoundedRectangle(D2D1::RoundedRect(m_timelineTrackRect, 2.5f, 2.5f), brush.Get());

    const float playRatio = (keptDurSec > 1e-4)
        ? static_cast<float>(std::clamp(m_playheadSec / keptDurSec, 0.0, 1.0))
        : 0.0f;
    const float scrubPlayX = m_timelineTrackRect.left +
        (m_timelineTrackRect.right - m_timelineTrackRect.left) * playRatio;
    setBrush(colAccent);
    m_renderTarget->FillRoundedRectangle(
        D2D1::RoundedRect(D2D1::RectF(
            m_timelineTrackRect.left, m_timelineTrackRect.top,
            (std::max)(m_timelineTrackRect.left + 2.5f, scrubPlayX), m_timelineTrackRect.bottom), 2.5f, 2.5f),
        brush.Get());
    m_renderTarget->FillEllipse(D2D1::Ellipse({scrubPlayX, (m_timelineTrackRect.top + m_timelineTrackRect.bottom) * 0.5f}, 4.5f, 4.5f), brush.Get());

    const float trackW = (std::max)(1.0f, laneRight - laneLeft);
    const double keptTotalMs = (std::max)(50.0, keptDurSec * 1000.0);

    // 涵盖整片轨道的悬停实时掠览区 (刻度尺 + 复合视音轨 + 运镜轨 + 标注轨)
    m_allLanesHoverRect = D2D1::RectF(laneLeft, tlCard.top + 35.0f, laneRight, tlCard.top + 172.0f);
    m_hitZones.push_back({Hit_TimelineRulerScrub, D2D1::RectF(laneLeft, tlCard.top + 35.0f, laneRight, tlCard.top + 54.0f), -1});

    // 精密广播级时间标尺 (Precision NLE Broadcast Time Ruler: 自适应整秒步长 + 5 等分次级微刻度齿)
    {
        const float rulerBottomY = tlCard.top + 53.0f;
        setBrush(dark ? D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.06f) : D2D1::ColorF(0.06f, 0.09f, 0.16f, 0.06f));
        m_renderTarget->DrawLine({laneLeft, rulerBottomY}, {laneRight, rulerBottomY}, brush.Get(), 1.0f);

        static constexpr double kNiceSteps[] = {
            0.25, 0.5, 1.0, 2.0, 5.0, 10.0, 15.0, 30.0, 60.0, 120.0, 300.0, 600.0
        };
        const double safeSpanSec = (std::max)(0.25, keptDurSec);
        const int maxMajorTicks = (std::max)(4, static_cast<int>(trackW / 92.0f));
        double majorStepSec = kNiceSteps[std::size(kNiceSteps) - 1];
        for (double cand : kNiceSteps) {
            if (safeSpanSec / cand <= static_cast<double>(maxMajorTicks)) {
                majorStepSec = cand;
                break;
            }
        }
        const double subStepSec = majorStepSec / 5.0;
        const int totalSubTicks = (std::min)(400, static_cast<int>(std::floor(safeSpanSec / subStepSec + 1e-6)));
        for (int si = 0; si <= totalSubTicks; ++si) {
            const double tSec = static_cast<double>(si) * subStepSec;
            const float ratio = static_cast<float>(std::clamp(tSec / safeSpanSec, 0.0, 1.0));
            const float tx = laneLeft + trackW * ratio;
            const bool isMajor = (si % 5 == 0);
            if (isMajor) {
                setBrush(dark ? D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.24f) : D2D1::ColorF(0.10f, 0.15f, 0.25f, 0.24f));
                m_renderTarget->DrawLine({tx, rulerBottomY - 6.0f}, {tx, rulerBottomY}, brush.Get(), 1.1f);

                wchar_t sTxt[28]{};
                if (safeSpanSec >= 60.0) {
                    const int mins = static_cast<int>(tSec) / 60;
                    const int secs = static_cast<int>(std::round(std::fmod(tSec, 60.0)));
                    std::swprintf(sTxt, std::size(sTxt), L"%02d:%02d", mins, secs % 60);
                } else if (majorStepSec < 1.0) {
                    std::swprintf(sTxt, std::size(sTxt), L"%.2fs", tSec);
                } else {
                    std::swprintf(sTxt, std::size(sTxt), L"%.1fs", tSec);
                }
                const float lblLeft = std::clamp(tx - 24.0f, laneLeft - 4.0f, laneRight - 44.0f);
                const DWRITE_TEXT_ALIGNMENT align = (si == 0)
                    ? DWRITE_TEXT_ALIGNMENT_LEADING
                    : ((tx > laneRight - 22.0f) ? DWRITE_TEXT_ALIGNMENT_TRAILING : DWRITE_TEXT_ALIGNMENT_CENTER);
                drawTextAligned(
                    sTxt, m_monoFmt.Get(),
                    D2D1::RectF(lblLeft, tlCard.top + 34.0f, lblLeft + 48.0f, rulerBottomY - 6.0f),
                    colSecondary, align);
            } else {
                setBrush(dark ? D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.11f) : D2D1::ColorF(0.10f, 0.15f, 0.25f, 0.11f));
                m_renderTarget->DrawLine({tx, rulerBottomY - 3.2f}, {tx, rulerBottomY}, brush.Get(), 0.9f);
            }
        }
    }

    // 轨道 1: 世界级 NLE 复合视音频主轨 (56px 充裕高度：上舱 17px 片段标题彩带 + 下舱 37px 专业双极镜像 RMS/Peak 声学频谱槽)
    m_cutLaneRect = D2D1::RectF(laneLeft, tlCard.top + 55.0f, laneRight, tlCard.top + 111.0f);
    {
        // 左侧轨道头：声波矢量徽标 + 轨道名
        const float hdrMidY = (m_cutLaneRect.top + m_cutLaneRect.bottom) * 0.5f;
        const float icX = tlCard.left + 14.0f;
        const float barH[5] = {3.0f, 6.5f, 4.2f, 8.0f, 3.6f};
        setBrush(colAccent);
        for (int bi = 0; bi < 5; ++bi) {
            const float bx = icX + static_cast<float>(bi) * 2.7f;
            m_renderTarget->FillRoundedRectangle(
                D2D1::RoundedRect(D2D1::RectF(bx, hdrMidY - 9.0f - barH[bi], bx + 1.6f, hdrMidY - 9.0f + barH[bi]), 0.8f, 0.8f),
                brush.Get());
        }
        drawTextAligned(isZh ? L"视频/音轨" : L"Video/Aud", m_captionFmt.Get(),
                        D2D1::RectF(tlCard.left + 8.0f, hdrMidY - 1.0f, laneLeft - 4.0f, m_cutLaneRect.bottom), colSecondary);
    }

    // 轨道底衬槽
    setBrush(dark ? D2D1::ColorF(0.035f, 0.045f, 0.072f, 0.92f) : D2D1::ColorF(0.89f, 0.91f, 0.95f, 0.85f));
    m_renderTarget->FillRoundedRectangle(D2D1::RoundedRect(m_cutLaneRect, 6.5f, 6.5f), brush.Get());
    setBrush(colBorder);
    m_renderTarget->DrawRoundedRectangle(D2D1::RoundedRect(m_cutLaneRect, 6.5f, 6.5f), brush.Get(), 0.9f);

    const float lanePlayX = laneLeft + trackW * playRatio;
    const int64_t rawTotalWaveMs = (std::max<int64_t>)(
        100, static_cast<int64_t>(std::llround(m_durationSec * 1000.0)));
    int64_t accKeptMs = 0;
    for (size_t i = 0; i < m_videoClips.size(); ++i) {
        const auto& clip = m_videoClips[i];
        const int64_t cDurMs = clip.keptDurationMs();
        const float sx = laneLeft + static_cast<float>(std::clamp(accKeptMs / keptTotalMs, 0.0, 1.0)) * trackW;
        const float ex = laneLeft + static_cast<float>(std::clamp((accKeptMs + cDurMs) / keptTotalMs, 0.0, 1.0)) * trackW;
        accKeptMs += cDurMs;

        const float gapL = (i == 0) ? 1.0f : 1.8f;
        const float gapR = (i + 1 == m_videoClips.size()) ? 1.0f : 1.8f;
        const D2D1_RECT_F clipRc = D2D1::RectF(
            sx + gapL, m_cutLaneRect.top + 1.0f,
            (std::max)(sx + gapL + 8.0f, ex - gapR), m_cutLaneRect.bottom - 1.0f);
        m_hitZones.push_back({Hit_VideoClipBase + static_cast<int>(i), clipRc, static_cast<int>(i)});

        const bool sel = (m_selectedClipIdx == static_cast<int>(i));
        const float headerStripH = 17.5f;
        const float headerBottom = clipRc.top + headerStripH;

        // 1. 片段主体底板（深邃精密暗舱，彻底告别刺眼全紫大色块）
        const D2D1_COLOR_F clipWellCol = dark
            ? (sel ? D2D1::ColorF(0.085f, 0.115f, 0.185f, 0.98f) : D2D1::ColorF(0.058f, 0.078f, 0.128f, 0.96f))
            : (sel ? D2D1::ColorF(0.93f, 0.96f, 1.00f, 0.98f)   : D2D1::ColorF(0.955f, 0.968f, 0.988f, 0.96f));
        setBrush(clipWellCol);
        m_renderTarget->FillRoundedRectangle(D2D1::RoundedRect(clipRc, 5.5f, 5.5f), brush.Get());

        // 使用轴对齐裁剪确保上舱标题彩带与声波严格收束于卡片内部
        m_renderTarget->PushAxisAlignedClip(clipRc, D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);

        // 2. 上舱：专业 NLE 片段标题彩带 (Header Strip — 与下方波形区物理隔离，文字与波形 0 重叠！)
        const D2D1_RECT_F hdrRc = D2D1::RectF(clipRc.left, clipRc.top, clipRc.right, headerBottom);
        const D2D1_COLOR_F hdrBg = dark
            ? (sel ? D2D1::ColorF(0.28f, 0.36f, 0.68f, 0.88f) : D2D1::ColorF(0.18f, 0.24f, 0.44f, 0.80f))
            : (sel ? D2D1::ColorF(0.22f, 0.44f, 0.88f, 0.90f) : D2D1::ColorF(0.27f, 0.48f, 0.84f, 0.82f));
        setBrush(hdrBg);
        m_renderTarget->FillRectangle(hdrRc, brush.Get());

        setBrush(dark ? D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.12f) : D2D1::ColorF(0.0f, 0.0f, 0.0f, 0.08f));
        m_renderTarget->DrawLine({clipRc.left, headerBottom}, {clipRc.right, headerBottom}, brush.Get(), 0.9f);

        const float cw = clipRc.right - clipRc.left;
        if (cw >= 38.0f) {
            // 左侧迷你胶片色标点
            setBrush(D2D1::ColorF(0.45f, 0.84f, 1.0f, 0.95f));
            m_renderTarget->FillRoundedRectangle(
                D2D1::RoundedRect(D2D1::RectF(clipRc.left + 6.0f, clipRc.top + 4.5f, clipRc.left + 9.0f, headerBottom - 4.5f), 1.5f, 1.5f),
                brush.Get());

            wchar_t cLbl[64]{};
            if (cw >= 108.0f) {
                std::swprintf(cLbl, std::size(cLbl),
                              isZh ? L"片段 %zu · %.1fs" : L"Clip %zu · %.1fs",
                              i + 1, cDurMs / 1000.0);
            } else {
                std::swprintf(cLbl, std::size(cLbl), L"%.1fs", cDurMs / 1000.0);
            }
            const bool showSpdPill = (cw >= 88.0f) || (std::abs(clip.speed - 1.0f) > 0.03f && cw >= 62.0f);
            const float rightReserve = showSpdPill ? 36.0f : 5.0f;
            drawTextAligned(cLbl, m_captionFmt.Get(),
                            D2D1::RectF(clipRc.left + 13.0f, clipRc.top, clipRc.right - rightReserve, headerBottom),
                            D2D1::ColorF(0.97f, 0.98f, 1.0f, 0.96f));

            if (showSpdPill) {
                wchar_t spdChip[20]{};
                std::swprintf(spdChip, std::size(spdChip), L"%.1fx", clip.speed);
                const D2D1_RECT_F pillRc = D2D1::RectF(clipRc.right - 33.0f, clipRc.top + 2.2f, clipRc.right - 5.0f, headerBottom - 2.2f);
                const bool fastSpd = std::abs(clip.speed - 1.0f) > 0.03f;
                setBrush(fastSpd ? D2D1::ColorF(0.08f, 0.78f, 0.66f, 0.88f) : D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.16f));
                m_renderTarget->FillRoundedRectangle(D2D1::RoundedRect(pillRc, 3.5f, 3.5f), brush.Get());
                drawTextAligned(spdChip, m_monoFmt.Get(), pillRc, D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.98f), DWRITE_TEXT_ALIGNMENT_CENTER);
            }
        }

        // 3. 下舱：专业双极中心对称声学频谱槽 (Symmetric Mirrored Peak + RMS Capsule Waveform)
        const float waveTop = headerBottom + 2.0f;
        const float waveBot = clipRc.bottom - 2.0f;
        const float waveMidY = (waveTop + waveBot) * 0.5f;
        const float maxHalfAmp = (std::max)(3.0f, (waveBot - waveTop) * 0.5f - 1.0f); // ~15.2px 半振幅 (总高 ~30.4px)
        const float waveLeft = clipRc.left + 5.0f;
        const float waveRight = clipRc.right - 5.0f;

        if (waveRight > waveLeft + 2.0f) {
            // 0 dB / -inf 声学零交越基准细轴线
            setBrush(dark ? D2D1::ColorF(0.56f, 0.68f, 0.88f, 0.14f) : D2D1::ColorF(0.20f, 0.30f, 0.48f, 0.13f));
            m_renderTarget->DrawLine({waveLeft, waveMidY}, {waveRight, waveMidY}, brush.Get(), 0.85f);

            if (m_hasActiveAudioWaveform && !m_timeline.audioWaveformPeaks.empty()) {
                constexpr float kBarPitchPx = 3.0f; // 1.75px 圆头声波柱 + 1.25px 精密间隙
                constexpr float kBarWidthPx = 1.75f;
                const float spanPx = waveRight - waveLeft;
                const int barCount = (std::max)(1, static_cast<int>(std::floor(spanPx / kBarPitchPx)));
                const int64_t clipSrcSpanMs = (std::max<int64_t>)(1, clip.srcEndMs - clip.srcStartMs);

                std::vector<float> peakLevels(static_cast<size_t>(barCount), 0.0f);
                for (int bi = 0; bi < barCount; ++bi) {
                    const double u0 = std::clamp<double>((static_cast<double>(bi) * kBarPitchPx) / spanPx, 0.0, 1.0);
                    const double u1 = std::clamp<double>(((static_cast<double>(bi) + 1.0) * kBarPitchPx) / spanPx, u0, 1.0);
                    const int64_t sMs = clip.srcStartMs + static_cast<int64_t>(std::llround(u0 * clipSrcSpanMs));
                    const int64_t eMs = clip.srcStartMs + static_cast<int64_t>(std::llround(u1 * clipSrcSpanMs));
                    peakLevels[static_cast<size_t>(bi)] = StudioTimelineAnalyzer::sampleWaveformRangePeak(
                        m_timeline.audioWaveformPeaks, rawTotalWaveMs, sMs, eMs, m_waveformReferencePeak);
                }

                // 3-Tap 汉宁核计算平滑 RMS 能量内芯，使相邻声波柱兼具瞬态锐度与流体韵律感
                for (int bi = 0; bi < barCount; ++bi) {
                    const float pPrev = peakLevels[static_cast<size_t>((std::max)(0, bi - 1))];
                    const float pCur  = peakLevels[static_cast<size_t>(bi)];
                    const float pNext = peakLevels[static_cast<size_t>((std::min)(barCount - 1, bi + 1))];
                    const float smoothEnv = 0.22f * pPrev + 0.56f * pCur + 0.22f * pNext;
                    const float peakEnv = (std::max)(pCur, smoothEnv);

                    const float cxBar = waveLeft + (static_cast<float>(bi) + 0.5f) * kBarPitchPx;
                    const bool passed = (cxBar <= lanePlayX);

                    if (peakEnv < 0.014f) {
                        // 静音气口：极简微晶零轴脉冲点，直观暴露说话换气停顿位供一键按 S 剪气口
                        const D2D1_RECT_F dotRc = D2D1::RectF(
                            cxBar - 0.65f, waveMidY - 0.65f, cxBar + 0.65f, waveMidY + 0.65f);
                        setBrush(passed
                            ? (dark ? D2D1::ColorF(0.32f, 0.82f, 0.96f, 0.36f) : D2D1::ColorF(0.14f, 0.54f, 0.86f, 0.36f))
                            : (dark ? D2D1::ColorF(0.54f, 0.66f, 0.84f, 0.22f) : D2D1::ColorF(0.32f, 0.42f, 0.58f, 0.22f)));
                        m_renderTarget->FillRoundedRectangle(D2D1::RoundedRect(dotRc, 0.65f, 0.65f), brush.Get());
                        continue;
                    }

                    const float halfPeak = std::clamp(1.3f + peakEnv * (maxHalfAmp - 1.3f), 1.3f, maxHalfAmp);
                    const float halfRms  = std::clamp(0.9f + smoothEnv * 0.64f * (maxHalfAmp - 0.9f), 0.9f, halfPeak);
                    const bool hotTransient = (peakEnv > 0.90f);

                    // 外层：瞬态峰值柔光包络柱 (Peak Envelope)
                    const D2D1_RECT_F outerBar = D2D1::RectF(
                        cxBar - kBarWidthPx * 0.5f, waveMidY - halfPeak,
                        cxBar + kBarWidthPx * 0.5f, waveMidY + halfPeak);
                    const D2D1_COLOR_F outerCol = passed
                        ? (dark ? D2D1::ColorF(0.22f, 0.78f, 0.98f, 0.44f) : D2D1::ColorF(0.08f, 0.56f, 0.92f, 0.40f))
                        : (dark ? D2D1::ColorF(0.26f, 0.58f, 0.86f, 0.28f) : D2D1::ColorF(0.20f, 0.46f, 0.78f, 0.26f));
                    setBrush(outerCol);
                    m_renderTarget->FillRoundedRectangle(
                        D2D1::RoundedRect(outerBar, kBarWidthPx * 0.5f, kBarWidthPx * 0.5f), brush.Get());

                    // 内层：高对比均方根能量实心核 (RMS Acoustic Core)
                    const D2D1_RECT_F coreBar = D2D1::RectF(
                        cxBar - kBarWidthPx * 0.46f, waveMidY - halfRms,
                        cxBar + kBarWidthPx * 0.46f, waveMidY + halfRms);
                    D2D1_COLOR_F coreCol;
                    if (hotTransient && passed) {
                        coreCol = dark ? D2D1::ColorF(0.32f, 0.95f, 0.82f, 0.98f) : D2D1::ColorF(0.04f, 0.66f, 0.56f, 0.96f);
                    } else if (passed) {
                        coreCol = dark ? D2D1::ColorF(0.29f, 0.85f, 0.99f, 0.95f) : D2D1::ColorF(0.05f, 0.52f, 0.90f, 0.92f);
                    } else {
                        coreCol = dark ? D2D1::ColorF(0.36f, 0.68f, 0.92f, 0.66f) : D2D1::ColorF(0.22f, 0.48f, 0.78f, 0.62f);
                    }
                    setBrush(coreCol);
                    m_renderTarget->FillRoundedRectangle(
                        D2D1::RoundedRect(coreBar, kBarWidthPx * 0.46f, kBarWidthPx * 0.46f), brush.Get());
                }
            } else {
                // 无音轨或纯静音录屏：优雅展示居中静音状态提示微标
                if (cw >= 120.0f) {
                    drawTextAligned(
                        isZh ? L"无音频轨道 / 静音片段" : L"Muted / No Audio Track",
                        m_captionFmt.Get(),
                        D2D1::RectF(waveLeft, waveTop, waveRight, waveBot),
                        D2D1::ColorF(colSecondary.r, colSecondary.g, colSecondary.b, 0.48f),
                        DWRITE_TEXT_ALIGNMENT_CENTER);
                }
            }
        }

        m_renderTarget->PopAxisAlignedClip();

        // 4. 片段外边框（选中态亮青白双层微晶聚焦环，常态细腻冷灰边）
        if (sel) {
            setBrush(D2D1::ColorF(0.36f, 0.84f, 1.0f, 0.96f));
            m_renderTarget->DrawRoundedRectangle(D2D1::RoundedRect(clipRc, 5.5f, 5.5f), brush.Get(), 1.8f);
        } else {
            setBrush(dark ? D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.14f) : D2D1::ColorF(0.10f, 0.18f, 0.34f, 0.18f));
            m_renderTarget->DrawRoundedRectangle(D2D1::RoundedRect(clipRc, 5.5f, 5.5f), brush.Get(), 1.0f);
        }
    }

    // 轨道 2: 蓝色自动运镜轨 (115..141，带左右微晶修剪抓手刻痕)
    m_zoomLaneRect = D2D1::RectF(laneLeft, tlCard.top + 115.0f, laneRight, tlCard.top + 141.0f);
    drawTextAligned(isZh ? L"自动运镜" : L"Auto Zoom", m_captionFmt.Get(),
                    D2D1::RectF(tlCard.left + 8.0f, m_zoomLaneRect.top, laneLeft - 4.0f, m_zoomLaneRect.bottom), colSecondary);
    m_hitZones.push_back({Hit_ZoomLaneEmpty, m_zoomLaneRect, -1});
    setBrush(dark ? D2D1::ColorF(0.035f, 0.045f, 0.072f, 0.86f) : colChipBg);
    m_renderTarget->FillRoundedRectangle(D2D1::RoundedRect(m_zoomLaneRect, 5.0f, 5.0f), brush.Get());

    bool drewAnyZoom = false;
    for (size_t i = 0; i < m_timeline.zoomSegments.size(); ++i) {
        const auto& seg = m_timeline.zoomSegments[i];
        const int64_t ks = StudioTimelineAnalyzer::mapSourceMsToKeptMsViaClips(m_videoClips, seg.startMs);
        const int64_t ke = StudioTimelineAnalyzer::mapSourceMsToKeptMsViaClips(m_videoClips, seg.endMs);
        if (ke <= ks + 30) continue; // 完全落在已删除切片中的运镜自动随废片隐去
        drewAnyZoom = true;

        const float sx = laneLeft + static_cast<float>(std::clamp(ks / keptTotalMs, 0.0, 1.0)) * trackW;
        const float ex = laneLeft + static_cast<float>(std::clamp(ke / keptTotalMs, 0.0, 1.0)) * trackW;
        const D2D1_RECT_F segRc = D2D1::RectF(sx, m_zoomLaneRect.top + 2.0f, (std::max)(sx + 16.0f, ex), m_zoomLaneRect.bottom - 2.0f);
        m_hitZones.push_back({Hit_ZoomSegmentBase + static_cast<int>(i), segRc, static_cast<int>(i)});
        const bool sel = (m_selectedZoomIdx == static_cast<int>(i));

        setBrush((seg.enabled && m_config.autoZoomEnabled)
            ? D2D1::ColorF(0.16f, 0.48f, 0.96f, sel ? 0.96f : 0.80f)
            : D2D1::ColorF(0.45f, 0.52f, 0.64f, 0.28f));
        m_renderTarget->FillRoundedRectangle(D2D1::RoundedRect(segRc, 5.0f, 5.0f), brush.Get());
        if (sel) {
            setBrush(D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.95f));
            m_renderTarget->DrawRoundedRectangle(D2D1::RoundedRect(segRc, 5.0f, 5.0f), brush.Get(), 1.6f);
        }
        const float zw = segRc.right - segRc.left;
        if (zw >= 24.0f) {
            setBrush(D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.45f));
            m_renderTarget->DrawLine({segRc.left + 3.2f, segRc.top + 5.5f}, {segRc.left + 3.2f, segRc.bottom - 5.5f}, brush.Get(), 1.2f);
            m_renderTarget->DrawLine({segRc.right - 3.2f, segRc.top + 5.5f}, {segRc.right - 3.2f, segRc.bottom - 5.5f}, brush.Get(), 1.2f);
        }
        if (zw >= 42.0f) {
            const float effZoom = seg.manualInserted ? seg.targetZoom : m_config.defaultZoomFactor;
            wchar_t zLbl[32]{};
            if (zw >= 74.0f) {
                std::swprintf(zLbl, std::size(zLbl), isZh ? L"运镜 %.1fx" : L"Zoom %.1fx", effZoom);
            } else {
                std::swprintf(zLbl, std::size(zLbl), L"%.1fx", effZoom);
            }
            drawTextAligned(zLbl, m_captionFmt.Get(), segRc,
                            D2D1::ColorF(1, 1, 1, 0.96f), DWRITE_TEXT_ALIGNMENT_CENTER);
        }
    }
    if (!drewAnyZoom) {
        drawTextAligned(isZh ? L"点击此轨空白处直接插入自动运镜" : L"Click anywhere on track to add zoom",
                        m_captionFmt.Get(),
                        D2D1::RectF(laneLeft + 8.0f, m_zoomLaneRect.top, laneRight - 8.0f, m_zoomLaneRect.bottom), colSecondary);
    }

    // 轨道 3: 琥珀橙视频标注与字幕轨 (145..171)
    m_annLaneRect = D2D1::RectF(laneLeft, tlCard.top + 145.0f, laneRight, tlCard.top + 171.0f);
    drawTextAligned(isZh ? L"标注/字幕" : L"Mark", m_captionFmt.Get(),
                    D2D1::RectF(tlCard.left + 8.0f, m_annLaneRect.top, laneLeft - 4.0f, m_annLaneRect.bottom), colSecondary);
    setBrush(dark ? D2D1::ColorF(0.035f, 0.045f, 0.072f, 0.86f) : colChipBg);
    m_renderTarget->FillRoundedRectangle(D2D1::RoundedRect(m_annLaneRect, 5.0f, 5.0f), brush.Get());

    for (const auto& cap : m_timeline.captions) {
        const int64_t ks = StudioTimelineAnalyzer::mapSourceMsToKeptMsViaClips(m_videoClips, cap.startMs);
        const int64_t ke = StudioTimelineAnalyzer::mapSourceMsToKeptMsViaClips(m_videoClips, cap.endMs);
        if (ke <= ks + 30) continue;
        const float sx = laneLeft + static_cast<float>(std::clamp(ks / keptTotalMs, 0.0, 1.0)) * trackW;
        const float ex = laneLeft + static_cast<float>(std::clamp(ke / keptTotalMs, 0.0, 1.0)) * trackW;
        setBrush(m_config.autoCaptionsEnabled
            ? D2D1::ColorF(0.22f, 0.74f, 0.97f, 0.75f)
            : D2D1::ColorF(0.45f, 0.52f, 0.64f, 0.25f));
        m_renderTarget->FillRoundedRectangle(
            D2D1::RoundedRect(D2D1::RectF(sx, m_annLaneRect.top + 14.0f, (std::max)(sx + 8.0f, ex), m_annLaneRect.bottom - 2.0f), 2.5f, 2.5f),
            brush.Get());
    }
    for (size_t i = 0; i < m_timeline.annotations.size(); ++i) {
        const auto& ann = m_timeline.annotations[i];
        const int64_t ks = StudioTimelineAnalyzer::mapSourceMsToKeptMsViaClips(m_videoClips, ann.startMs);
        const int64_t ke = StudioTimelineAnalyzer::mapSourceMsToKeptMsViaClips(m_videoClips, ann.endMs);
        if (ke <= ks + 30) continue;
        const float sx = laneLeft + static_cast<float>(std::clamp(ks / keptTotalMs, 0.0, 1.0)) * trackW;
        const float ex = laneLeft + static_cast<float>(std::clamp(ke / keptTotalMs, 0.0, 1.0)) * trackW;
        const D2D1_RECT_F arc = D2D1::RectF(sx, m_annLaneRect.top + 2.0f, (std::max)(sx + 16.0f, ex), m_annLaneRect.bottom - 2.0f);
        m_hitZones.push_back({Hit_AnnSegmentBase + static_cast<int>(i), arc, static_cast<int>(i)});
        const bool sel = (m_selectedAnnIdx == static_cast<int>(i));
        setBrush(D2D1::ColorF(0.95f, 0.56f, 0.08f, sel ? 0.96f : 0.84f));
        m_renderTarget->FillRoundedRectangle(D2D1::RoundedRect(arc, 5.0f, 5.0f), brush.Get());
        if (sel) {
            setBrush(D2D1::ColorF(1, 1, 1, 0.96f));
            m_renderTarget->DrawRoundedRectangle(D2D1::RoundedRect(arc, 5.0f, 5.0f), brush.Get(), 1.6f);
        }
    }

    // 悬停实时掠览银色游标线 (Live Skimmer Line) + 秒数浮标
    if (m_hoverSkimming && keptDurSec > 1e-4) {
        const float skimRatio = static_cast<float>(std::clamp(m_hoverSkimSec / keptDurSec, 0.0, 1.0));
        const float skimX = laneLeft + trackW * skimRatio;
        setBrush(dark ? D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.72f) : D2D1::ColorF(0.15f, 0.20f, 0.32f, 0.65f));
        m_renderTarget->DrawLine({skimX, m_cutLaneRect.top - 8.0f}, {skimX, m_annLaneRect.bottom + 2.0f}, brush.Get(), 1.2f);

        wchar_t skBuf[24]{};
        std::swprintf(skBuf, std::size(skBuf), L"%.1fs", m_hoverSkimSec);
        const D2D1_RECT_F badgeRc = D2D1::RectF(
            std::clamp(skimX - 22.0f, laneLeft, laneRight - 44.0f),
            tlCard.top + 35.0f,
            std::clamp(skimX - 22.0f, laneLeft, laneRight - 44.0f) + 44.0f,
            tlCard.top + 51.0f);
        setBrush(dark ? D2D1::ColorF(0.16f, 0.20f, 0.30f, 0.96f) : D2D1::ColorF(0.12f, 0.16f, 0.24f, 0.92f));
        m_renderTarget->FillRoundedRectangle(D2D1::RoundedRect(badgeRc, 4.0f, 4.0f), brush.Get());
        drawTextAligned(skBuf, m_monoFmt.Get(), badgeRc, D2D1::ColorF(1, 1, 1, 1), DWRITE_TEXT_ALIGNMENT_CENTER);
    }

    // 贯穿三轨的专业 NLE 五边形钻石盾徽播放指针 (Diamond-Crest Playhead + Glow Sword Line)
    {
        setBrush(D2D1::ColorF(0.0f, 0.0f, 0.0f, 0.36f));
        m_renderTarget->DrawLine({lanePlayX, m_cutLaneRect.top - 3.0f}, {lanePlayX, m_annLaneRect.bottom + 3.0f}, brush.Get(), 3.6f);

        setBrush(D2D1::ColorF(0.99f, 0.56f, 0.12f, 0.98f));
        m_renderTarget->DrawLine({lanePlayX, m_cutLaneRect.top - 3.0f}, {lanePlayX, m_annLaneRect.bottom + 3.0f}, brush.Get(), 1.75f);

        ComPtr<ID2D1PathGeometry> crestGeo;
        if (m_d2dFactory && SUCCEEDED(m_d2dFactory->CreatePathGeometry(&crestGeo)) && crestGeo) {
            ComPtr<ID2D1GeometrySink> sink;
            if (SUCCEEDED(crestGeo->Open(&sink)) && sink) {
                const float ct = m_cutLaneRect.top - 11.0f;
                sink->BeginFigure({lanePlayX - 5.2f, ct}, D2D1_FIGURE_BEGIN_FILLED);
                sink->AddLine({lanePlayX + 5.2f, ct});
                sink->AddLine({lanePlayX + 5.2f, ct + 5.8f});
                sink->AddLine({lanePlayX, ct + 10.6f});
                sink->AddLine({lanePlayX - 5.2f, ct + 5.8f});
                sink->EndFigure(D2D1_FIGURE_END_CLOSED);
                sink->Close();
                m_renderTarget->FillGeometry(crestGeo.Get(), brush.Get());
                setBrush(D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.88f));
                m_renderTarget->DrawGeometry(crestGeo.Get(), brush.Get(), 0.9f);
            }
        }
    }

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

    // 补齐高频滑动节流挂起的最后一帧，确保停手瞬间 100% 精确对齐当前毫秒画面
    if (m_pendingSkimCompose) {
        m_pendingSkimCompose = false;
        m_lastComposeQpcTick = queryQpcNow();
        updateCurrentComposedBitmap();
        InvalidateRect(m_hwnd, nullptr, FALSE);
        return;
    }

    if (m_exporting.load(std::memory_order_acquire) || m_editingExportName || m_editingAnnotationText) {
        InvalidateRect(m_hwnd, nullptr, FALSE);
        return;
    }

    // 悬停在时间轴上实时掠览看点时不推进主播放头，画面完全跟随鼠标所指位置
    if (m_hoverSkimming) {
        return;
    }

    const double keptDurSec = computeEffectiveDurationSec();
    if (m_playing && !m_exportModalOpen && !m_scrubbingTimeline &&
        m_trackDragKind == TrackDragKind::None && m_stageAnnDrag == StageAnnDragKind::None && keptDurSec > 0.05) {
        m_playheadSec += dt;
        if (m_playheadSec >= keptDurSec) {
            m_playheadSec = 0.0;
        }
        updateCurrentComposedBitmap();
        InvalidateRect(m_hwnd, nullptr, FALSE);
    }
}

void StudioPreviewWindow::startAsyncExport() {
    if (m_exporting.exchange(true, std::memory_order_acq_rel)) return;

    m_playing = false;
    m_hoverSkimming = false;
    m_editingExportName = false;
    m_openDropdown = ExportDropdownKind::None;
    m_cancelExport.store(false, std::memory_order_release);
    m_exportProgress.store(0.01, std::memory_order_release);
    syncCutSegmentsFromVideoClips();
    persistStudioConfigToSettings(m_config);
    persistExportPreferencesIfChecked();
    InvalidateRect(m_hwnd, nullptr, FALSE);

    joinExportWorkerIfDone(true);

    const std::string rawPath = m_rawVideoPath;
    const std::string finalPath = buildFinalExportPathUtf8();
    const StudioTimelineData timelineCopy = m_timeline;
    const StudioRenderConfig configCopy = m_config;
    const StudioExportOptions exportOptsCopy = m_exportOpts;
    const double durSec = computeEffectiveDurationSec();

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
    m_hoverSkimming = false;
    m_pendingSkimCompose = false;
    m_trackingMouseLeave = false;
    m_exportModalOpen = false;
    m_editingExportName = false;
    m_editingAnnotationText = false;
    m_openDropdown = ExportDropdownKind::None;
    m_scrubbingTimeline = false;
    m_draggingWindow = false;
    m_stageAnnDrag = StageAnnDragKind::None;
    m_trackDragKind = TrackDragKind::None;
    m_activeSlider = SliderDragKind::None;
    m_undoStack.clear();
    m_redoStack.clear();
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
    joinFileDialogWorkerIfDone(true);
    closeSessionAndCleanup();
    if (m_hwnd && IsWindow(m_hwnd)) {
        DestroyWindow(m_hwnd);
    }
    m_hwnd = nullptr;
}

void StudioPreviewWindow::applySliderAtX(SliderDragKind kind, float lx) {
    const auto& trk = m_sliderTrackRects[static_cast<size_t>(kind)];
    const float w = (std::max)(1.0f, trk.right - trk.left);
    const float ratio = std::clamp((lx - trk.left) / w, 0.0f, 1.0f);

    if (kind == SliderDragKind::Padding) {
        m_config.paddingRatio = ratio * 0.16f;
        m_config.autoFrameEnabled = true;
    } else if (kind == SliderDragKind::InsetBorder) {
        m_config.insetBorderWidth = std::round(ratio * 12.0f);
        m_config.autoFrameEnabled = true;
    } else if (kind == SliderDragKind::Radius) {
        m_config.cornerRadius = std::round(ratio * 36.0f);
        m_config.autoFrameEnabled = true;
    } else if (kind == SliderDragKind::Shadow) {
        m_config.shadowIntensity = ratio;
        m_config.autoFrameEnabled = true;
    } else if (kind == SliderDragKind::BgBlur) {
        m_config.backgroundBlur = std::round(ratio * 40.0f);
        m_config.autoFrameEnabled = true;
    }
    requestImmediatePreviewRefresh(false);
}

void StudioPreviewWindow::handleMouseWheel(float lx, float ly, int wheelDelta) {
    if (m_exportModalOpen || wheelDelta == 0) return;
    if (m_activeTab == LeftRailTab::CanvasBg && pointInRectF(m_wallpaperGridRect, lx, ly)) {
        const int perPage = static_cast<int>(kWallpaperSlotsPerPage);
        const int totalPages = (std::max)(1, static_cast<int>((static_cast<int>(m_filteredWallpaperIndices.size()) + perPage - 1) / perPage));
        if (totalPages > 1) {
            m_wallpaperPage = (wheelDelta < 0)
                ? ((m_wallpaperPage + 1) % totalPages)
                : ((m_wallpaperPage - 1 + totalPages) % totalPages);
            invalidateWallpaperPageBitmaps();
            InvalidateRect(m_hwnd, nullptr, FALSE);
        }
    }
}

void StudioPreviewWindow::handleMouseLeave() {
    m_trackingMouseLeave = false;
    if (m_hoverSkimming) {
        m_hoverSkimming = false;
        requestImmediatePreviewRefresh(true);
    }
}

void StudioPreviewWindow::handleRightMouseDown(float lx, float ly) {
    if (m_exportModalOpen || m_exporting.load(std::memory_order_acquire)) return;

    for (auto it = m_hitZones.rbegin(); it != m_hitZones.rend(); ++it) {
        if (!pointInRectF(it->rect, lx, ly)) continue;
        const int id = it->id;
        if (id >= Hit_AnnSegmentBase) {
            const int idx = it->payload;
            if (idx >= 0 && idx < static_cast<int>(m_timeline.annotations.size())) {
                pushUndoSnapshot();
                m_timeline.annotations.erase(m_timeline.annotations.begin() + idx);
                m_selectedAnnIdx = -1;
                m_editingAnnotationText = false;
                requestImmediatePreviewRefresh(true);
            }
            return;
        }
        if (id >= Hit_VideoClipBase && id < Hit_AnnSegmentBase) {
            const int idx = it->payload;
            if (idx >= 0 && idx < static_cast<int>(m_videoClips.size()) && m_videoClips.size() > 1) {
                m_selectedClipIdx = idx;
                m_selectedZoomIdx = -1;
                m_selectedAnnIdx = -1;
                deleteSelectedTimelineEntity();
            }
            return;
        }
        if (id >= Hit_ZoomSegmentBase && id < Hit_VideoClipBase) {
            const int idx = it->payload;
            if (idx >= 0 && idx < static_cast<int>(m_timeline.zoomSegments.size())) {
                pushUndoSnapshot();
                m_timeline.zoomSegments.erase(m_timeline.zoomSegments.begin() + idx);
                m_selectedZoomIdx = -1;
                rebakeTimelinePreview();
                requestImmediatePreviewRefresh(true);
            }
            return;
        }
    }
}

void StudioPreviewWindow::handleMouseDown(float lx, float ly, bool isDoubleClick) {
    const double keptDurSec = computeEffectiveDurationSec();

    for (auto it = m_hitZones.rbegin(); it != m_hitZones.rend(); ++it) {
        if (!pointInRectF(it->rect, lx, ly)) continue;

        const int id = it->id;

        // ── 导出模态窗热区处理 ──
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
            browseExportDirectoryAsync();
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

        if (id != Hit_AnnTextInputBox && id != Hit_StageCanvas) {
            m_editingAnnotationText = false;
        }

        // ── 顶栏与导航热区 ──
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
            m_hoverSkimming = false;
            m_exportModalOpen = true;
            m_editingExportName = false;
            m_openDropdown = ExportDropdownKind::None;
            InvalidateRect(m_hwnd, nullptr, FALSE);
            return;
        }
        if (id >= Hit_LeftRailTabBase && id < Hit_LeftRailTabBase + 4) {
            m_activeTab = static_cast<LeftRailTab>(it->payload);
            InvalidateRect(m_hwnd, nullptr, FALSE);
            return;
        }
        if (id == Hit_QuickOpenAnnotations) {
            m_activeTab = LeftRailTab::Annotations;
            InvalidateRect(m_hwnd, nullptr, FALSE);
            return;
        }
        if (id == Hit_SplitAtPlayhead) {
            splitClipAtCurrentCursor(false);
            return;
        }
        if (id == Hit_DeleteSelectedItem) {
            deleteSelectedTimelineEntity();
            return;
        }
        if (id == Hit_CycleClipSpeed) {
            cycleSelectedClipSpeed();
            return;
        }
        if (id == Hit_UndoBtn) {
            performUndo();
            return;
        }
        if (id == Hit_RedoBtn) {
            performRedo();
            return;
        }
        if (id == Hit_PlayPauseBtn) {
            m_hoverSkimming = false;
            m_playing = !m_playing;
            m_lastTickMs = GetTickCount();
            requestImmediatePreviewRefresh(true);
            return;
        }
        if (id == Hit_TimelineTopBarScrub || id == Hit_TimelineRulerScrub) {
            m_playing = false;
            m_hoverSkimming = false;
            m_scrubbingTimeline = true;
            m_scrubOnLaneArea = (id == Hit_TimelineRulerScrub);
            SetCapture(m_hwnd);
            const D2D1_RECT_F& refRc = m_scrubOnLaneArea ? m_cutLaneRect : m_timelineTrackRect;
            const float w = (std::max)(1.0f, refRc.right - refRc.left);
            const double ratio = std::clamp(static_cast<double>((lx - refRc.left) / w), 0.0, 1.0);
            m_playheadSec = ratio * keptDurSec;
            requestImmediatePreviewRefresh(true);
            return;
        }

        // ── 视频标注卡片与属性热区 ──
        if (id >= Hit_AnnCardBase && id < Hit_AnnCardBase + 19) {
            insertAnnotationCardPreset(it->payload);
            return;
        }
        if (id >= Hit_AnnColorBase && id < Hit_AnnColorBase + static_cast<int>(kAnnotationPaletteHex.size())) {
            if (m_selectedAnnIdx >= 0 && m_selectedAnnIdx < static_cast<int>(m_timeline.annotations.size())) {
                pushUndoSnapshot();
                m_timeline.annotations[static_cast<size_t>(m_selectedAnnIdx)].colorHex =
                    kAnnotationPaletteHex[static_cast<size_t>(it->payload)];
                requestImmediatePreviewRefresh(true);
            }
            return;
        }
        if (id == Hit_AnnTextInputBox) {
            m_playing = false;
            m_editingAnnotationText = true;
            InvalidateRect(m_hwnd, nullptr, FALSE);
            return;
        }

        // ── 预览舞台点击：拖拽/缩放可见标注，或点击重设选中运镜段焦点 ──
        if (id == Hit_StageCanvas) {
            float nx = 0.5f, ny = 0.5f;
            if (!mapStagePointToSourceNorm(lx, ly, nx, ny)) return;

            const int64_t curSrcMs = static_cast<int64_t>(std::llround(currentDisplayedSourceSec() * 1000.0));
            for (int ai = static_cast<int>(m_timeline.annotations.size()) - 1; ai >= 0; --ai) {
                const auto& ann = m_timeline.annotations[static_cast<size_t>(ai)];
                if (!ann.enabled || curSrcMs < ann.startMs || curSrcMs > ann.endMs) continue;

                float p0x = 0.0f, p0y = 0.0f, p1x = 0.0f, p1y = 0.0f;
                if (!mapSourceNormToStagePoint(ann.x0, ann.y0, p0x, p0y) ||
                    !mapSourceNormToStagePoint(ann.x1, ann.y1, p1x, p1y)) {
                    continue;
                }
                const float handleX = isPointDirectedAnnotation(ann.kind) ? p1x : (std::max)(p0x, p1x) + 4.0f;
                const float handleY = isPointDirectedAnnotation(ann.kind) ? p1y : (std::max)(p0y, p1y) + 4.0f;
                if (!isTextLikeAnnotation(ann.kind) && std::hypot(lx - handleX, ly - handleY) <= 13.0f) {
                    pushUndoSnapshot();
                    m_playing = false;
                    m_selectedAnnIdx = ai;
                    m_selectedZoomIdx = -1;
                    m_selectedClipIdx = -1;
                    m_stageAnnDrag = StageAnnDragKind::DragEndPoint;
                    m_annDragStartNormX = nx;
                    m_annDragStartNormY = ny;
                    m_annOrigX0 = ann.x0; m_annOrigY0 = ann.y0;
                    m_annOrigX1 = ann.x1; m_annOrigY1 = ann.y1;
                    SetCapture(m_hwnd);
                    InvalidateRect(m_hwnd, nullptr, FALSE);
                    return;
                }

                const D2D1_RECT_F hitBox = D2D1::RectF(
                    (std::min)(p0x, p1x) - 22.0f, (std::min)(p0y, p1y) - 18.0f,
                    (std::max)(p0x, p1x) + 22.0f, (std::max)(p0y, p1y) + 18.0f);
                if (pointInRectF(hitBox, lx, ly)) {
                    m_playing = false;
                    m_selectedAnnIdx = ai;
                    m_selectedZoomIdx = -1;
                    m_selectedClipIdx = -1;
                    m_activeTab = LeftRailTab::Annotations;
                    if (isDoubleClick && isTextLikeAnnotation(ann.kind)) {
                        m_editingAnnotationText = true;
                    } else {
                        pushUndoSnapshot();
                        m_stageAnnDrag = StageAnnDragKind::MoveBody;
                        m_annDragStartNormX = nx;
                        m_annDragStartNormY = ny;
                        m_annOrigX0 = ann.x0; m_annOrigY0 = ann.y0;
                        m_annOrigX1 = ann.x1; m_annOrigY1 = ann.y1;
                        SetCapture(m_hwnd);
                    }
                    InvalidateRect(m_hwnd, nullptr, FALSE);
                    return;
                }
            }

            if (m_selectedZoomIdx >= 0 && m_selectedZoomIdx < static_cast<int>(m_timeline.zoomSegments.size())) {
                pushUndoSnapshot();
                auto& seg = m_timeline.zoomSegments[static_cast<size_t>(m_selectedZoomIdx)];
                seg.focusX = nx * static_cast<float>((std::max)(1, m_timeline.sourceWidth));
                seg.focusY = ny * static_cast<float>((std::max)(1, m_timeline.sourceHeight));
                rebakeTimelinePreview();
                requestImmediatePreviewRefresh(true);
            }
            return;
        }

        // ── 点击紫色视频切片：选中该切片 + 定格橙色主指针 + 支持按住左键继续丝滑拖指针看画面！ ──
        if (id >= Hit_VideoClipBase && id < Hit_AnnSegmentBase) {
            const int clipIdx = it->payload;
            m_playing = false;
            m_hoverSkimming = false;
            m_selectedClipIdx = clipIdx;
            m_selectedZoomIdx = -1;
            m_selectedAnnIdx = -1;
            m_scrubbingTimeline = true;
            m_scrubOnLaneArea = true;
            SetCapture(m_hwnd);

            const float w = (std::max)(1.0f, m_cutLaneRect.right - m_cutLaneRect.left);
            const double ratio = std::clamp(static_cast<double>((lx - m_cutLaneRect.left) / w), 0.0, 1.0);
            m_playheadSec = ratio * keptDurSec;
            requestImmediatePreviewRefresh(true);
            return;
        }

        // ── 时间轴插入运镜与轨道拖拽 ──
        const int64_t rawTotalMs = (std::max<int64_t>)(400, static_cast<int64_t>(std::llround(m_durationSec * 1000.0)));
        if (id == Hit_AddZoomAtPlayhead || id == Hit_ZoomLaneEmpty) {
            pushUndoSnapshot();
            m_playing = false;
            m_hoverSkimming = false;
            const float laneW = (std::max)(1.0f, m_zoomLaneRect.right - m_zoomLaneRect.left);
            const int64_t clickKeptMs = (id == Hit_ZoomLaneEmpty)
                ? static_cast<int64_t>(std::llround(std::clamp((lx - m_zoomLaneRect.left) / laneW, 0.0f, 1.0f) * keptDurSec * 1000.0))
                : static_cast<int64_t>(std::llround(m_playheadSec * 1000.0));
            const int64_t centerSrcMs = StudioTimelineAnalyzer::mapKeptMsToSourceMs(m_videoClips, clickKeptMs);
            const int64_t halfSpan = (std::min<int64_t>)(1100, rawTotalMs / 3);
            int64_t st = std::clamp<int64_t>(centerSrcMs - halfSpan / 2, 0, (std::max<int64_t>)(0, rawTotalMs - 300));
            int64_t ed = (std::min)(rawTotalMs, st + (std::max<int64_t>)(400, halfSpan * 2));

            const float srcW = static_cast<float>((std::max)(1, m_timeline.sourceWidth));
            const float srcH = static_cast<float>((std::max)(1, m_timeline.sourceHeight));
            float fxPx = srcW * 0.5f, fyPx = srcH * 0.5f;
            if (!m_evaluatedFrames.empty()) {
                const size_t eIdx = (std::min)(
                    m_evaluatedFrames.size() - 1,
                    static_cast<size_t>(std::llround((st / 1000.0) * m_previewEvalFps)));
                fxPx = std::clamp(m_evaluatedFrames[eIdx].smoothCursorX, srcW * 0.12f, srcW * 0.88f);
                fyPx = std::clamp(m_evaluatedFrames[eIdx].smoothCursorY, srcH * 0.12f, srcH * 0.88f);
            }

            StudioZoomSegment z{};
            z.startMs = st;
            z.endMs = ed;
            z.focusX = fxPx;
            z.focusY = fyPx;
            z.targetZoom = m_config.defaultZoomFactor;
            z.enabled = true;
            z.manualInserted = true;
            m_config.autoZoomEnabled = true;
            if (m_config.zoomMode == StudioZoomMode::None) {
                m_config.zoomMode = StudioZoomMode::Zoom2D;
            }
            m_timeline.zoomSegments.push_back(z);
            m_selectedZoomIdx = static_cast<int>(m_timeline.zoomSegments.size()) - 1;
            m_selectedClipIdx = -1;
            m_selectedAnnIdx = -1;
            m_playheadSec = std::clamp(clickKeptMs / 1000.0, 0.0, keptDurSec);
            rebakeTimelinePreview();
            requestImmediatePreviewRefresh(true);
            return;
        }

        if (id >= Hit_AnnSegmentBase) {
            const int idx = it->payload;
            if (idx >= 0 && idx < static_cast<int>(m_timeline.annotations.size())) {
                m_playing = false;
                m_hoverSkimming = false;
                m_selectedAnnIdx = idx;
                m_selectedZoomIdx = -1;
                m_selectedClipIdx = -1;
                m_activeTab = LeftRailTab::Annotations;
                auto& ann = m_timeline.annotations[static_cast<size_t>(idx)];
                if (isDoubleClick && isTextLikeAnnotation(ann.kind)) {
                    m_editingAnnotationText = true;
                } else {
                    pushUndoSnapshot();
                    const float edgeTol = (std::min)(7.0f, (it->rect.right - it->rect.left) * 0.28f);
                    m_trackDragKind = (lx <= it->rect.left + edgeTol) ? TrackDragKind::AnnLeft
                        : ((lx >= it->rect.right - edgeTol) ? TrackDragKind::AnnRight : TrackDragKind::AnnMove);
                    m_trackDragIndex = idx;
                    m_trackDragStartX = lx;
                    m_trackDragOrigKeptStartMs = StudioTimelineAnalyzer::mapSourceMsToKeptMsViaClips(m_videoClips, ann.startMs);
                    m_trackDragOrigKeptEndMs = StudioTimelineAnalyzer::mapSourceMsToKeptMsViaClips(m_videoClips, ann.endMs);
                    SetCapture(m_hwnd);
                }
                m_playheadSec = std::clamp(
                    StudioTimelineAnalyzer::mapSourceMsToKeptMsViaClips(m_videoClips, ann.startMs) / 1000.0 + 0.05,
                    0.0, keptDurSec);
                requestImmediatePreviewRefresh(true);
            }
            return;
        }
        if (id >= Hit_ZoomSegmentBase && id < Hit_VideoClipBase) {
            const int idx = it->payload;
            if (idx >= 0 && idx < static_cast<int>(m_timeline.zoomSegments.size())) {
                m_playing = false;
                m_hoverSkimming = false;
                m_selectedZoomIdx = idx;
                m_selectedClipIdx = -1;
                m_selectedAnnIdx = -1;
                auto& zseg = m_timeline.zoomSegments[static_cast<size_t>(idx)];
                pushUndoSnapshot();
                if (isDoubleClick) {
                    zseg.enabled = !zseg.enabled;
                    rebakeTimelinePreview();
                } else {
                    const float edgeTol = (std::min)(7.0f, (it->rect.right - it->rect.left) * 0.28f);
                    m_trackDragKind = (lx <= it->rect.left + edgeTol) ? TrackDragKind::ZoomLeft
                        : ((lx >= it->rect.right - edgeTol) ? TrackDragKind::ZoomRight : TrackDragKind::ZoomMove);
                    m_trackDragIndex = idx;
                    m_trackDragStartX = lx;
                    m_trackDragOrigKeptStartMs = StudioTimelineAnalyzer::mapSourceMsToKeptMsViaClips(m_videoClips, zseg.startMs);
                    m_trackDragOrigKeptEndMs = StudioTimelineAnalyzer::mapSourceMsToKeptMsViaClips(m_videoClips, zseg.endMs);
                    SetCapture(m_hwnd);
                }
                m_playheadSec = std::clamp(
                    StudioTimelineAnalyzer::mapSourceMsToKeptMsViaClips(m_videoClips, zseg.startMs) / 1000.0 + 0.08,
                    0.0, keptDurSec);
                requestImmediatePreviewRefresh(true);
            }
            return;
        }

        // ── 抽屉面板控制项响应 ──
        if (id == Hit_ZoomMode_None || id == Hit_ZoomMode_2D || id == Hit_ZoomMode_3D) {
            if (id == Hit_ZoomMode_None) {
                m_config.autoZoomEnabled = false;
                m_config.zoomMode = StudioZoomMode::None;
            } else {
                m_config.autoZoomEnabled = true;
                m_config.zoomMode = (id == Hit_ZoomMode_3D) ? StudioZoomMode::Motion3D : StudioZoomMode::Zoom2D;
            }
            rebakeTimelinePreview();
            requestImmediatePreviewRefresh(true);
            return;
        }
        if (id == Hit_ToggleMotionBlur) {
            m_config.motionBlurEnabled = !m_config.motionBlurEnabled;
            requestImmediatePreviewRefresh(true);
            return;
        }
        if (id == Hit_ZoomPreset_150 || id == Hit_ZoomPreset_185 || id == Hit_ZoomPreset_220) {
            const float z = (id == Hit_ZoomPreset_150) ? 1.50f : ((id == Hit_ZoomPreset_185) ? 1.85f : 2.20f);
            m_config.defaultZoomFactor = z;
            m_config.autoZoomEnabled = true;
            if (m_config.zoomMode == StudioZoomMode::None) m_config.zoomMode = StudioZoomMode::Zoom2D;
            if (m_selectedZoomIdx >= 0 && m_selectedZoomIdx < static_cast<int>(m_timeline.zoomSegments.size())) {
                m_timeline.zoomSegments[static_cast<size_t>(m_selectedZoomIdx)].targetZoom = z;
            } else {
                for (auto& seg : m_timeline.zoomSegments) seg.targetZoom = z;
            }
            rebakeTimelinePreview();
            requestImmediatePreviewRefresh(true);
            return;
        }
        if (id == Hit_ToggleSmoothCursor) {
            m_config.smoothCursorEnabled = !m_config.smoothCursorEnabled;
            rebakeTimelinePreview();
            requestImmediatePreviewRefresh(true);
            return;
        }
        if (id == Hit_CursorScale_100 || id == Hit_CursorScale_135 || id == Hit_CursorScale_170) {
            m_config.cursorScale = (id == Hit_CursorScale_100) ? 1.00f
                : ((id == Hit_CursorScale_135) ? 1.35f : 1.70f);
            m_config.smoothCursorEnabled = true;
            requestImmediatePreviewRefresh(true);
            return;
        }
        if (id == Hit_ToggleCursorHalo) {
            m_config.cursorHighlightHalo = !m_config.cursorHighlightHalo;
            m_config.smoothCursorEnabled = true;
            requestImmediatePreviewRefresh(true);
            return;
        }
        if (id == Hit_ToggleAutoFrame) {
            m_config.autoFrameEnabled = !m_config.autoFrameEnabled;
            requestImmediatePreviewRefresh(true);
            return;
        }
        if (id >= Hit_AspectBase && id < Hit_AspectBase + 5) {
            m_config.aspectRatio = static_cast<StudioAspectRatio>(id - Hit_AspectBase);
            requestImmediatePreviewRefresh(true);
            return;
        }
        if (id == Hit_CycleInsetBorderColor) {
            size_t nextIdx = 0;
            for (size_t c = 0; c < kInsetBorderColors.size(); ++c) {
                if (m_config.insetBorderColorHex == kInsetBorderColors[c]) {
                    nextIdx = (c + 1) % kInsetBorderColors.size();
                    break;
                }
            }
            m_config.insetBorderColorHex = kInsetBorderColors[nextIdx];
            if (m_config.insetBorderWidth < 1.0f) m_config.insetBorderWidth = 2.0f;
            m_config.autoFrameEnabled = true;
            requestImmediatePreviewRefresh(true);
            return;
        }
        if (id == Hit_SliderPadding || id == Hit_SliderInsetBorder ||
            id == Hit_SliderRadius || id == Hit_SliderShadow || id == Hit_SliderBgBlur) {
            m_activeSlider = (id == Hit_SliderPadding) ? SliderDragKind::Padding
                : (id == Hit_SliderInsetBorder) ? SliderDragKind::InsetBorder
                : (id == Hit_SliderRadius) ? SliderDragKind::Radius
                : (id == Hit_SliderShadow) ? SliderDragKind::Shadow
                : SliderDragKind::BgBlur;
            SetCapture(m_hwnd);
            applySliderAtX(m_activeSlider, lx);
            return;
        }
        if (id >= Hit_WallCatBase && id < Hit_WallCatBase + 5) {
            m_wallpaperCategory = id - Hit_WallCatBase;
            m_wallpaperPage = 0;
            rebuildFilteredWallpapers();
            InvalidateRect(m_hwnd, nullptr, FALSE);
            return;
        }
        if (id == Hit_WallPrevPage || id == Hit_WallNextPage) {
            const int perPage = static_cast<int>(kWallpaperSlotsPerPage);
            const int totalPages = (std::max)(1, static_cast<int>((static_cast<int>(m_filteredWallpaperIndices.size()) + perPage - 1) / perPage));
            m_wallpaperPage = (id == Hit_WallPrevPage)
                ? ((m_wallpaperPage - 1 + totalPages) % totalPages)
                : ((m_wallpaperPage + 1) % totalPages);
            invalidateWallpaperPageBitmaps();
            InvalidateRect(m_hwnd, nullptr, FALSE);
            return;
        }
        if (id == Hit_WallPickCustom) {
            browseCustomWallpaperImageAsync();
            return;
        }
        if (id >= Hit_ThemeSlotBase && id < Hit_ThemeSlotBase + static_cast<int>(kWallpaperSlotsPerPage)) {
            const int entryIdx = it->payload;
            if (entryIdx >= 0 && entryIdx < static_cast<int>(m_allWallpapers.size())) {
                const auto& entry = m_allWallpapers[static_cast<size_t>(entryIdx)];
                if (entry.isBuiltin) {
                    m_config.backgroundTheme = entry.builtinTheme;
                    m_config.customBackgroundPath.clear();
                } else {
                    m_config.customBackgroundPath = entry.filePathUtf8;
                }
                m_config.autoFrameEnabled = true;
                requestImmediatePreviewRefresh(true);
            }
            return;
        }
        if (id == Hit_ToggleCaptions) {
            m_config.autoCaptionsEnabled = !m_config.autoCaptionsEnabled;
            rebakeTimelinePreview();
            requestImmediatePreviewRefresh(true);
            return;
        }
    }

    if (!m_exportModalOpen && ly <= 44.0f && m_hwnd) {
        m_draggingWindow = true;
        SetCapture(m_hwnd);
        GetCursorPos(&m_dragStartScreen);
        RECT wr{};
        GetWindowRect(m_hwnd, &wr);
        m_windowStartPos = {wr.left, wr.top};
    }
}

void StudioPreviewWindow::handleMouseMove(float lx, float ly, bool leftButtonDown) {
    if (!m_trackingMouseLeave && m_hwnd) {
        TRACKMOUSEEVENT tme{};
        tme.cbSize = sizeof(tme);
        tme.dwFlags = TME_LEAVE;
        tme.hwndTrack = m_hwnd;
        if (TrackMouseEvent(&tme)) {
            m_trackingMouseLeave = true;
        }
    }

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

    if (m_activeSlider != SliderDragKind::None && leftButtonDown) {
        applySliderAtX(m_activeSlider, lx);
        return;
    }

    if (m_stageAnnDrag != StageAnnDragKind::None && leftButtonDown &&
        m_selectedAnnIdx >= 0 && m_selectedAnnIdx < static_cast<int>(m_timeline.annotations.size())) {
        float nx = 0.5f, ny = 0.5f;
        if (mapStagePointToSourceNorm(lx, ly, nx, ny)) {
            auto& ann = m_timeline.annotations[static_cast<size_t>(m_selectedAnnIdx)];
            const float dx = nx - m_annDragStartNormX;
            const float dy = ny - m_annDragStartNormY;
            if (m_stageAnnDrag == StageAnnDragKind::MoveBody) {
                const float w = m_annOrigX1 - m_annOrigX0;
                const float h = m_annOrigY1 - m_annOrigY0;
                ann.x0 = std::clamp(m_annOrigX0 + dx, 0.0f, 1.0f);
                ann.y0 = std::clamp(m_annOrigY0 + dy, 0.0f, 1.0f);
                ann.x1 = std::clamp(ann.x0 + w, 0.0f, 1.0f);
                ann.y1 = std::clamp(ann.y0 + h, 0.0f, 1.0f);
            } else {
                ann.x1 = std::clamp(nx, 0.02f, 0.98f);
                ann.y1 = std::clamp(ny, 0.02f, 0.98f);
            }
            requestImmediatePreviewRefresh(false);
        }
        return;
    }

    const double keptDurSec = computeEffectiveDurationSec();
    const int64_t keptTotalMs = (std::max<int64_t>)(100, static_cast<int64_t>(std::llround(keptDurSec * 1000.0)));

    if (m_trackDragKind != TrackDragKind::None && leftButtonDown) {
        const float laneW = (std::max)(1.0f, m_zoomLaneRect.right - m_zoomLaneRect.left);
        const int64_t deltaKeptMs = static_cast<int64_t>(std::llround(((lx - m_trackDragStartX) / laneW) * keptTotalMs));
        const int64_t minSpan = 220;

        auto applyKeptDragToSourceBounds = [&](int64_t& srcSt, int64_t& srcEd, TrackDragKind moveK, TrackDragKind leftK) -> int64_t {
            int64_t newKeptSt = m_trackDragOrigKeptStartMs;
            int64_t newKeptEd = m_trackDragOrigKeptEndMs;
            if (m_trackDragKind == moveK) {
                const int64_t len = (std::max<int64_t>)(minSpan, m_trackDragOrigKeptEndMs - m_trackDragOrigKeptStartMs);
                newKeptSt = std::clamp<int64_t>(m_trackDragOrigKeptStartMs + deltaKeptMs, 0, (std::max<int64_t>)(0, keptTotalMs - len));
                newKeptEd = newKeptSt + len;
            } else if (m_trackDragKind == leftK) {
                newKeptSt = std::clamp<int64_t>(m_trackDragOrigKeptStartMs + deltaKeptMs, 0, m_trackDragOrigKeptEndMs - minSpan);
            } else {
                newKeptEd = std::clamp<int64_t>(m_trackDragOrigKeptEndMs + deltaKeptMs, m_trackDragOrigKeptStartMs + minSpan, keptTotalMs);
            }
            srcSt = StudioTimelineAnalyzer::mapKeptMsToSourceMs(m_videoClips, newKeptSt);
            srcEd = StudioTimelineAnalyzer::mapKeptMsToSourceMs(m_videoClips, newKeptEd);
            if (srcEd <= srcSt + minSpan) {
                srcEd = (std::min<int64_t>)(static_cast<int64_t>(std::llround(m_durationSec * 1000.0)), srcSt + minSpan);
            }
            return (m_trackDragKind == TrackDragKind::ZoomRight || m_trackDragKind == TrackDragKind::AnnRight)
                ? newKeptEd : newKeptSt;
        };

        if (m_trackDragKind == TrackDragKind::ZoomMove ||
            m_trackDragKind == TrackDragKind::ZoomLeft ||
            m_trackDragKind == TrackDragKind::ZoomRight) {
            if (m_trackDragIndex >= 0 && m_trackDragIndex < static_cast<int>(m_timeline.zoomSegments.size())) {
                auto& z = m_timeline.zoomSegments[static_cast<size_t>(m_trackDragIndex)];
                const int64_t previewKeptMs = applyKeptDragToSourceBounds(
                    z.startMs, z.endMs, TrackDragKind::ZoomMove, TrackDragKind::ZoomLeft);
                m_playheadSec = std::clamp(previewKeptMs / 1000.0, 0.0, keptDurSec);
                rebakeTimelinePreview();
                requestImmediatePreviewRefresh(false);
            }
        } else if (m_trackDragKind == TrackDragKind::AnnMove ||
                   m_trackDragKind == TrackDragKind::AnnLeft ||
                   m_trackDragKind == TrackDragKind::AnnRight) {
            if (m_trackDragIndex >= 0 && m_trackDragIndex < static_cast<int>(m_timeline.annotations.size())) {
                auto& a = m_timeline.annotations[static_cast<size_t>(m_trackDragIndex)];
                const int64_t previewKeptMs = applyKeptDragToSourceBounds(
                    a.startMs, a.endMs, TrackDragKind::AnnMove, TrackDragKind::AnnLeft);
                m_playheadSec = std::clamp(previewKeptMs / 1000.0, 0.0, keptDurSec);
                requestImmediatePreviewRefresh(false);
            }
        }
        return;
    }

    // 按住左键在进度条或轨道区域拖拽主指针 (Active Scrubbing)
    if (m_scrubbingTimeline && leftButtonDown) {
        const D2D1_RECT_F& refRc = m_scrubOnLaneArea ? m_cutLaneRect : m_timelineTrackRect;
        const float w = (std::max)(1.0f, refRc.right - refRc.left);
        const double ratio = std::clamp(static_cast<double>((lx - refRc.left) / w), 0.0, 1.0);
        m_playheadSec = ratio * keptDurSec;
        m_hoverSkimming = false;
        // 同步高亮拖动所经位置的视频切片
        int clipAtCursor = -1;
        StudioTimelineAnalyzer::mapKeptMsToSourceMs(
            m_videoClips, static_cast<int64_t>(std::llround(m_playheadSec * 1000.0)), &clipAtCursor);
        if (m_scrubOnLaneArea && clipAtCursor >= 0) {
            m_selectedClipIdx = clipAtCursor;
        }
        requestImmediatePreviewRefresh(false);
        return;
    }

    int newHover = Hit_None;
    int newZoomHover = -1;
    for (auto it = m_hitZones.rbegin(); it != m_hitZones.rend(); ++it) {
        if (pointInRectF(it->rect, lx, ly)) {
            newHover = it->id;
            if (it->id >= Hit_ZoomSegmentBase && it->id < Hit_VideoClipBase) {
                newZoomHover = it->payload;
            }
            break;
        }
    }

    // ── FocuSee 级无按键悬停实时轨道掠览 (Live Hover Skimming) ──
    bool skimChanged = false;
    if (!m_exportModalOpen && !leftButtonDown && pointInRectF(m_allLanesHoverRect, lx, ly)) {
        const float laneW = (std::max)(1.0f, m_allLanesHoverRect.right - m_allLanesHoverRect.left);
        const double ratio = std::clamp(static_cast<double>((lx - m_allLanesHoverRect.left) / laneW), 0.0, 1.0);
        const double targetSkimSec = ratio * keptDurSec;
        if (!m_hoverSkimming || std::abs(targetSkimSec - m_hoverSkimSec) > 0.002) {
            m_hoverSkimming = true;
            m_hoverSkimSec = targetSkimSec;
            skimChanged = true;
        }
    } else if (m_hoverSkimming) {
        m_hoverSkimming = false;
        skimChanged = true;
    }

    if (skimChanged) {
        m_hoveredHitId = newHover;
        m_hoveredZoomIdx = newZoomHover;
        requestImmediatePreviewRefresh(!m_hoverSkimming);
        return;
    }

    if (newHover != m_hoveredHitId || newZoomHover != m_hoveredZoomIdx) {
        m_hoveredHitId = newHover;
        m_hoveredZoomIdx = newZoomHover;
        InvalidateRect(m_hwnd, nullptr, FALSE);
    }
}

void StudioPreviewWindow::handleMouseUp() {
    const bool hadCapture = m_scrubbingTimeline || m_draggingWindow ||
                            m_activeSlider != SliderDragKind::None ||
                            m_trackDragKind != TrackDragKind::None ||
                            m_stageAnnDrag != StageAnnDragKind::None;
    m_scrubbingTimeline = false;
    m_scrubOnLaneArea = false;
    m_draggingWindow = false;
    m_activeSlider = SliderDragKind::None;
    m_trackDragKind = TrackDragKind::None;
    m_trackDragIndex = -1;
    m_stageAnnDrag = StageAnnDragKind::None;
    if (hadCapture) {
        ReleaseCapture();
        requestImmediatePreviewRefresh(true);
    }
}

void StudioPreviewWindow::handleCharInput(wchar_t ch) {
    if (m_exporting.load(std::memory_order_acquire)) return;

    if (m_exportModalOpen && m_editingExportName) {
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
        return;
    }

    if (!m_exportModalOpen && m_editingAnnotationText &&
        m_selectedAnnIdx >= 0 && m_selectedAnnIdx < static_cast<int>(m_timeline.annotations.size())) {
        auto& ann = m_timeline.annotations[static_cast<size_t>(m_selectedAnnIdx)];
        std::wstring wtxt = tools3000::core::WinUtils::utf8ToWstring(ann.text);
        if (ch == L'\b') {
            if (!wtxt.empty()) {
                wtxt.pop_back();
                ann.text = tools3000::core::WinUtils::wstringToUtf8(wtxt);
                requestImmediatePreviewRefresh(true);
            }
            return;
        }
        if (ch == L'\r' || ch == L'\n') {
            m_editingAnnotationText = false;
            InvalidateRect(m_hwnd, nullptr, FALSE);
            return;
        }
        if (ch >= 32 && wtxt.size() < 64) {
            wtxt.push_back(ch);
            ann.text = tools3000::core::WinUtils::wstringToUtf8(wtxt);
            requestImmediatePreviewRefresh(true);
        }
    }
}

void StudioPreviewWindow::handleKeyDown(WPARAM vk) {
    if (m_exporting.load(std::memory_order_acquire)) return;

    const bool ctrlDown = (GetAsyncKeyState(VK_CONTROL) & 0x8000) != 0;

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
        if (m_editingExportName && ctrlDown && vk == 'V') {
            if (OpenClipboard(m_hwnd)) {
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
            }
            return;
        }
        if (!m_editingExportName && vk == VK_RETURN) {
            startAsyncExport();
        }
        return;
    }

    if (m_editingAnnotationText) {
        if (vk == VK_ESCAPE) {
            m_editingAnnotationText = false;
            InvalidateRect(m_hwnd, nullptr, FALSE);
            return;
        }
        if (ctrlDown && vk == 'V' &&
            m_selectedAnnIdx >= 0 && m_selectedAnnIdx < static_cast<int>(m_timeline.annotations.size())) {
            if (OpenClipboard(m_hwnd)) {
                if (HANDLE hData = GetClipboardData(CF_UNICODETEXT)) {
                    if (const auto* wptr = static_cast<const wchar_t*>(GlobalLock(hData))) {
                        auto& ann = m_timeline.annotations[static_cast<size_t>(m_selectedAnnIdx)];
                        std::wstring cur = tools3000::core::WinUtils::utf8ToWstring(ann.text);
                        for (size_t i = 0; wptr[i] != L'\0' && cur.size() < 64; ++i) {
                            if (wptr[i] >= 32) cur.push_back(wptr[i]);
                        }
                        ann.text = tools3000::core::WinUtils::wstringToUtf8(cur);
                        GlobalUnlock(hData);
                    }
                }
                CloseClipboard();
                requestImmediatePreviewRefresh(true);
            }
            return;
        }
        return;
    }

    // Ctrl+E 或 Ctrl+Enter：一键呼出演播室导出配置模态框
    if (ctrlDown && (vk == 'E' || vk == VK_RETURN)) {
        m_playing = false;
        m_hoverSkimming = false;
        m_exportModalOpen = true;
        m_editingExportName = false;
        m_openDropdown = ExportDropdownKind::None;
        InvalidateRect(m_hwnd, nullptr, FALSE);
        return;
    }

    // Ctrl+Z 撤销 / Ctrl+Y 重做
    if (ctrlDown && vk == 'Z') {
        performUndo();
        return;
    }
    if (ctrlDown && vk == 'Y') {
        performRedo();
        return;
    }

    // S 或 Ctrl+B：在当前指针（若正在轨道上悬停掠览则优先在悬停位置）一刀拆分切片
    if ((!ctrlDown && vk == 'S') || (ctrlDown && vk == 'B')) {
        splitClipAtCurrentCursor(true);
        return;
    }

    if (vk == VK_DELETE || vk == VK_BACK) {
        deleteSelectedTimelineEntity();
        return;
    }

    if (vk == VK_SPACE) {
        m_hoverSkimming = false;
        m_playing = !m_playing;
        m_lastTickMs = GetTickCount();
        requestImmediatePreviewRefresh(true);
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
        case WM_STUDIO_CUSTOM_BG_PICKED:
            self->onCustomWallpaperPicked();
            return 0;
        case WM_STUDIO_EXPORT_DIR_PICKED:
            self->onExportDirectoryPicked();
            return 0;
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
        case WM_MOUSELEAVE:
            self->handleMouseLeave();
            return 0;
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
        case WM_MOUSEWHEEL: {
            POINT pt{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
            ScreenToClient(hwnd, &pt);
            const float s = (std::max)(1.0f, self->m_dpiScale);
            self->handleMouseWheel(pt.x / s, pt.y / s, GET_WHEEL_DELTA_WPARAM(wParam));
            return 0;
        }
        case WM_LBUTTONDOWN: {
            const float s = (std::max)(1.0f, self->m_dpiScale);
            self->handleMouseDown(GET_X_LPARAM(lParam) / s, GET_Y_LPARAM(lParam) / s, false);
            return 0;
        }
        case WM_LBUTTONDBLCLK: {
            const float s = (std::max)(1.0f, self->m_dpiScale);
            self->handleMouseDown(GET_X_LPARAM(lParam) / s, GET_Y_LPARAM(lParam) / s, true);
            return 0;
        }
        case WM_RBUTTONDOWN: {
            const float s = (std::max)(1.0f, self->m_dpiScale);
            self->handleRightMouseDown(GET_X_LPARAM(lParam) / s, GET_Y_LPARAM(lParam) / s);
            return 0;
        }
        case WM_MOUSEMOVE: {
            const float s = (std::max)(1.0f, self->m_dpiScale);
            const float lx = GET_X_LPARAM(lParam) / s;
            const float ly = GET_Y_LPARAM(lParam) / s;
            self->handleMouseMove(lx, ly, (wParam & MK_LBUTTON) != 0);

            HCURSOR cur = LoadCursorW(nullptr, IDC_ARROW);
            if (self->m_hoveredHitId == Hit_ModalNameInput || self->m_hoveredHitId == Hit_AnnTextInputBox) {
                cur = LoadCursorW(nullptr, IDC_IBEAM);
            } else if ((self->m_hoveredHitId >= Hit_ZoomSegmentBase && self->m_hoveredHitId < Hit_VideoClipBase) ||
                       self->m_hoveredHitId >= Hit_AnnSegmentBase) {
                for (const auto& hz : self->m_hitZones) {
                    if (hz.id == self->m_hoveredHitId) {
                        const float edgeTol = (std::min)(7.0f, (hz.rect.right - hz.rect.left) * 0.28f);
                        cur = (lx <= hz.rect.left + edgeTol || lx >= hz.rect.right - edgeTol)
                            ? LoadCursorW(nullptr, IDC_SIZEWE)
                            : LoadCursorW(nullptr, IDC_HAND);
                        break;
                    }
                }
            } else if (self->m_hoveredHitId != Hit_None &&
                       self->m_hoveredHitId != Hit_ModalBackdrop &&
                       self->m_hoveredHitId != Hit_ModalCardBody &&
                       self->m_hoveredHitId != Hit_StageCanvas) {
                cur = LoadCursorW(nullptr, IDC_HAND);
            }
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
