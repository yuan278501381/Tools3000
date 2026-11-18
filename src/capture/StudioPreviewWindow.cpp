#include "capture/StudioPreviewWindow.h"
#include "../resource.h"
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
#include <d2d1_1.h>
#include <dwmapi.h>
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
constexpr UINT WM_STUDIO_WATERMARK_IMG_PICKED = WM_APP + 63;

enum HitId : int {
    Hit_None = 0,
    Hit_CloseBtn = 1,
    Hit_PlayPauseBtn = 2,
    Hit_TimelineTopBarScrub = 3,
    Hit_TimelineRulerScrub = 4,
    Hit_StageCanvas = 5,
    Hit_MinimizeBtn = 6,
    Hit_MaxRestoreBtn = 7,

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
    Hit_ClickStyleBase = 42, // 42..45 对应 StudioClickEffectStyle 0..3 (微晶单波 / 柔雾触点 / 错峰涟漪 / 仅按压)

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

    // 企业与品牌水印抽屉控制项（专属无碰撞 ID 段 400..430）
    Hit_WatermarkToggle = 400,
    Hit_WatermarkModeBase = 401,   // 401..403 (ImageLogo / CornerText / TiledTraceability)
    Hit_WatermarkPickImage = 404,
    Hit_WatermarkAnchorBase = 405, // 405..409 (TopLeft / TopRight / BottomLeft / BottomRight / Center)
    Hit_WatermarkTextInput = 410,
    Hit_WatermarkSliderScale = 411,
    Hit_WatermarkSliderOpacity = 412,
    Hit_WatermarkTokenBase = 415,  // 415..418 (+用户名 / +设备名 / +日期时间 / +严禁外传)
    Hit_WatermarkRecentBase = 420, // 420..425 (6 槽历史常用 Logo 缩略卡片)

    // 一体化走带中枢与时间轴视野横向缩放舱 (431..440)
    Hit_TransportJumpStart = 431,
    Hit_TransportPrevFrame = 432,
    Hit_TransportNextFrame = 433,
    Hit_TimelineZoomFit    = 434,
    Hit_TimelineZoomOut    = 435,
    Hit_TimelineZoomIn     = 436,
    Hit_TimelineZoomSlider = 437,
    Hit_TimelinePanBar     = 438,

    // 导出模态窗与原位交付结果卡交互热区
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
    Hit_ModalAutoCloseCheck = 211,
    Hit_ModalResultCopyFileBtn = 212,
    Hit_ModalResultOpenPlayBtn = 213,
    Hit_ModalResultRevealBtn = 214,
    Hit_ModalResultExportMoreBtn = 215,
    Hit_ModalResultFinishCloseBtn = 216,
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
    if (sec >= 3600.0) {
        const int64_t totalSecs = static_cast<int64_t>(std::llround(sec));
        const int64_t hrs = totalSecs / 3600;
        const int64_t mins = (totalSecs % 3600) / 60;
        const int64_t secs = totalSecs % 60;
        wchar_t buf[36]{};
        std::swprintf(buf, std::size(buf), L"%02lld:%02lld:%02lld",
                      static_cast<long long>(hrs),
                      static_cast<long long>(mins),
                      static_cast<long long>(secs));
        return buf;
    }
    const int totalTenths = static_cast<int>(std::round(sec * 10.0));
    const int mins = (totalTenths / 10) / 60;
    const int secs = (totalTenths / 10) % 60;
    const int tenths = totalTenths % 10;
    wchar_t buf[32]{};
    std::swprintf(buf, std::size(buf), L"%02d:%02d.%d", mins, secs, tenths);
    return buf;
}

std::wstring formatCompactDurationBadge(double sec, bool isZh) {
    if (sec < 0.0 || !std::isfinite(sec)) sec = 0.0;
    wchar_t buf[48]{};
    if (sec >= 3600.0) {
        const int64_t totalSecs = static_cast<int64_t>(std::llround(sec));
        const int64_t hrs = totalSecs / 3600;
        const int64_t mins = (totalSecs % 3600) / 60;
        const int64_t secs = totalSecs % 60;
        if (secs > 0) {
            std::swprintf(buf, std::size(buf), L"%lldh %02lldm %02llds",
                          static_cast<long long>(hrs), static_cast<long long>(mins), static_cast<long long>(secs));
        } else {
            std::swprintf(buf, std::size(buf), L"%lldh %02lldm",
                          static_cast<long long>(hrs), static_cast<long long>(mins));
        }
    } else if (sec >= 120.0) {
        const int totalSecs = static_cast<int>(std::round(sec));
        std::swprintf(buf, std::size(buf), isZh ? L"%d分%02d秒" : L"%dm %02ds", totalSecs / 60, totalSecs % 60);
    } else {
        std::swprintf(buf, std::size(buf), L"%.1fs", sec);
    }
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
    cm.set<int>("/recording/studioClickStyle", static_cast<int>(cfg.clickEffectStyle));
    cm.set<std::string>("/recording/studioCustomBg", cfg.customBackgroundPath);
    cm.set<std::string>("/recording/studioTheme", themeKeyFromEnum(cfg.backgroundTheme));
    cm.set<bool>("/recording/studioAutoCaptions", cfg.autoCaptionsEnabled);
    WatermarkRenderer::saveStudioWatermarkConfig(cfg.watermark);
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

ComPtr<ID2D1StrokeStyle> createRoundStrokeStyle(ID2D1Factory* factory) {
    ComPtr<ID2D1StrokeStyle> st;
    if (!factory) return st;
    D2D1_STROKE_STYLE_PROPERTIES props = D2D1::StrokeStyleProperties(
        D2D1_CAP_STYLE_ROUND, D2D1_CAP_STYLE_ROUND, D2D1_CAP_STYLE_ROUND,
        D2D1_LINE_JOIN_ROUND, 2.0f, D2D1_DASH_STYLE_SOLID, 0.0f);
    factory->CreateStrokeStyle(props, nullptr, 0, st.GetAddressOf());
    return st;
}

void fillLinearVGradRounded(ID2D1RenderTarget* rt, const D2D1_RECT_F& r, float radius,
                            const D2D1_COLOR_F& topCol, const D2D1_COLOR_F& botCol) {
    if (!rt || r.right <= r.left || r.bottom <= r.top) return;
    D2D1_GRADIENT_STOP stops[2] = {{0.0f, topCol}, {1.0f, botCol}};
    ComPtr<ID2D1GradientStopCollection> sc;
    if (SUCCEEDED(rt->CreateGradientStopCollection(stops, 2, D2D1_GAMMA_2_2, D2D1_EXTEND_MODE_CLAMP, &sc)) && sc) {
        ComPtr<ID2D1LinearGradientBrush> gb;
        if (SUCCEEDED(rt->CreateLinearGradientBrush(
                D2D1::LinearGradientBrushProperties({r.left, r.top}, {r.left, r.bottom}), sc.Get(), &gb)) && gb) {
            rt->FillRoundedRectangle(D2D1::RoundedRect(r, radius, radius), gb.Get());
        }
    }
}

void drawScissorsIcon(ID2D1RenderTarget* rt, ID2D1Brush* brush, float cx, float cy, ID2D1StrokeStyle* roundSt = nullptr) {
    rt->DrawEllipse(D2D1::Ellipse({cx - 3.6f, cy - 3.1f}, 2.15f, 2.15f), brush, 1.45f);
    rt->DrawEllipse(D2D1::Ellipse({cx - 3.6f, cy + 3.1f}, 2.15f, 2.15f), brush, 1.45f);
    rt->DrawLine({cx - 1.8f, cy - 1.7f}, {cx + 5.2f, cy + 3.6f}, brush, 1.5f, roundSt);
    rt->DrawLine({cx - 1.8f, cy + 1.7f}, {cx + 5.2f, cy - 3.6f}, brush, 1.5f, roundSt);
}

void drawCrispTrashIcon(ID2D1RenderTarget* rt, ID2D1Brush* brush, float cx, float cy, ID2D1StrokeStyle* roundSt) {
    rt->DrawLine({cx - 5.2f, cy - 3.2f}, {cx + 5.2f, cy - 3.2f}, brush, 1.45f, roundSt);
    rt->DrawLine({cx - 2.0f, cy - 3.2f}, {cx - 1.5f, cy - 5.0f}, brush, 1.35f, roundSt);
    rt->DrawLine({cx - 1.5f, cy - 5.0f}, {cx + 1.5f, cy - 5.0f}, brush, 1.35f, roundSt);
    rt->DrawLine({cx + 1.5f, cy - 5.0f}, {cx + 2.0f, cy - 3.2f}, brush, 1.35f, roundSt);
    const D2D1_RECT_F body = D2D1::RectF(cx - 3.8f, cy - 3.0f, cx + 3.8f, cy + 5.2f);
    rt->DrawRoundedRectangle(D2D1::RoundedRect(body, 1.4f, 1.4f), brush, 1.4f, roundSt);
    rt->DrawLine({cx - 1.3f, cy - 0.6f}, {cx - 1.3f, cy + 2.8f}, brush, 1.25f, roundSt);
    rt->DrawLine({cx + 1.3f, cy - 0.6f}, {cx + 1.3f, cy + 2.8f}, brush, 1.25f, roundSt);
}

void drawCrispSpeedIcon(ID2D1RenderTarget* rt, ID2D1Brush* brush, float cx, float cy, ID2D1StrokeStyle* roundSt) {
    for (int i = 0; i < 2; ++i) {
        const float ox = cx - 2.8f + i * 4.2f;
        rt->DrawLine({ox - 2.2f, cy - 3.6f}, {ox + 1.6f, cy}, brush, 1.55f, roundSt);
        rt->DrawLine({ox - 2.2f, cy + 3.6f}, {ox + 1.6f, cy}, brush, 1.55f, roundSt);
    }
}

void drawCrispUndoRedoIcon(ID2D1Factory* factory, ID2D1RenderTarget* rt, ID2D1Brush* brush,
                           float cx, float cy, bool isUndo, ID2D1StrokeStyle* roundSt) {
    const float dir = isUndo ? -1.0f : 1.0f;
    const float rU = 3.6f;
    const float topY = cy - rU + 0.5f;
    const float botY = cy + rU + 0.5f;
    const float bendX = cx - dir * 2.2f;
    ComPtr<ID2D1PathGeometry> geo;
    if (factory && SUCCEEDED(factory->CreatePathGeometry(&geo)) && geo) {
        ComPtr<ID2D1GeometrySink> sk;
        if (SUCCEEDED(geo->Open(&sk)) && sk) {
            sk->BeginFigure({cx + dir * 2.2f, botY}, D2D1_FIGURE_BEGIN_HOLLOW);
            sk->AddLine({bendX, botY});
            for (int s = 1; s <= 12; ++s) {
                const float ang = 1.5707963f + dir * (static_cast<float>(s) / 12.0f) * 3.1415926f;
                sk->AddLine({bendX + std::cos(ang) * rU, cy + 0.5f + std::sin(ang) * rU});
            }
            const D2D1_POINT_2F tip{cx + dir * 4.4f, topY};
            sk->AddLine(tip);
            sk->EndFigure(D2D1_FIGURE_END_OPEN);
            sk->Close();
            rt->DrawGeometry(geo.Get(), brush, 1.55f, roundSt);
            rt->DrawLine(tip, {tip.x - dir * 2.8f, tip.y - 2.5f}, brush, 1.5f, roundSt);
            rt->DrawLine(tip, {tip.x - dir * 2.8f, tip.y + 2.5f}, brush, 1.5f, roundSt);
        }
    }
}

void drawCrispPenIcon(ID2D1Factory* factory, ID2D1RenderTarget* rt, ID2D1Brush* brush,
                      float cx, float cy, ID2D1StrokeStyle* roundSt) {
    ComPtr<ID2D1PathGeometry> geo;
    if (factory && SUCCEEDED(factory->CreatePathGeometry(&geo)) && geo) {
        ComPtr<ID2D1GeometrySink> sk;
        if (SUCCEEDED(geo->Open(&sk)) && sk) {
            sk->BeginFigure({cx - 4.8f, cy + 4.8f}, D2D1_FIGURE_BEGIN_HOLLOW);
            sk->AddLine({cx - 4.0f, cy + 1.4f});
            sk->AddLine({cx + 2.4f, cy - 5.0f});
            sk->AddLine({cx + 5.0f, cy - 2.4f});
            sk->AddLine({cx - 1.4f, cy + 4.0f});
            sk->EndFigure(D2D1_FIGURE_END_CLOSED);
            sk->Close();
            rt->DrawGeometry(geo.Get(), brush, 1.45f, roundSt);
            rt->DrawLine({cx + 0.8f, cy - 3.4f}, {cx + 3.4f, cy - 0.8f}, brush, 1.3f, roundSt);
        }
    }
}

void drawCrispCameraFocusIcon(ID2D1RenderTarget* rt, ID2D1Brush* brush, float cx, float cy, ID2D1StrokeStyle* roundSt) {
    constexpr float s = 5.0f;
    constexpr float a = 2.4f;
    rt->DrawLine({cx - s, cy - s + a}, {cx - s, cy - s}, brush, 1.45f, roundSt);
    rt->DrawLine({cx - s, cy - s}, {cx - s + a, cy - s}, brush, 1.45f, roundSt);
    rt->DrawLine({cx + s - a, cy - s}, {cx + s, cy - s}, brush, 1.45f, roundSt);
    rt->DrawLine({cx + s, cy - s}, {cx + s, cy - s + a}, brush, 1.45f, roundSt);
    rt->DrawLine({cx - s, cy + s - a}, {cx - s, cy + s}, brush, 1.45f, roundSt);
    rt->DrawLine({cx - s, cy + s}, {cx - s + a, cy + s}, brush, 1.45f, roundSt);
    rt->DrawLine({cx + s - a, cy + s}, {cx + s, cy + s}, brush, 1.45f, roundSt);
    rt->DrawLine({cx + s, cy + s}, {cx + s, cy + s - a}, brush, 1.45f, roundSt);
    rt->DrawEllipse(D2D1::Ellipse({cx, cy}, 2.1f, 2.1f), brush, 1.45f);
}

void drawCrispExportIcon(ID2D1RenderTarget* rt, ID2D1Brush* brush, float cx, float cy, ID2D1StrokeStyle* roundSt) {
    rt->DrawLine({cx, cy + 2.2f}, {cx, cy - 4.5f}, brush, 1.65f, roundSt);
    rt->DrawLine({cx - 3.1f, cy - 1.5f}, {cx, cy - 4.6f}, brush, 1.65f, roundSt);
    rt->DrawLine({cx + 3.1f, cy - 1.5f}, {cx, cy - 4.6f}, brush, 1.65f, roundSt);
    rt->DrawLine({cx - 5.0f, cy + 2.0f}, {cx - 5.0f, cy + 4.6f}, brush, 1.55f, roundSt);
    rt->DrawLine({cx - 5.0f, cy + 4.6f}, {cx + 5.0f, cy + 4.6f}, brush, 1.55f, roundSt);
    rt->DrawLine({cx + 5.0f, cy + 4.6f}, {cx + 5.0f, cy + 2.0f}, brush, 1.55f, roundSt);
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

const wchar_t* annotationCardDisplayName(int cardIndex, bool isZh) noexcept {
    static const wchar_t* kZh[19] = {
        L"纯文字",     L"文字标签",   L"对话气泡",   L"虚线文本框", L"发光文本框", L"步骤序号",
        L"荧光划线",   L"虚线",       L"箭头",       L"矩形框",     L"圆角框",     L"圆环",
        L"手绘重点圈", L"醒目大箭头", L"点击手势",   L"聚光灯高亮", L"马赛克打码", L"局部放大镜",
        L"镜头推近缩放"
    };
    static const wchar_t* kEn[19] = {
        L"Plain Text",   L"Filled Label", L"Speech Bubble", L"Dashed Box",  L"Glow Callout", L"Step Number",
        L"Highlighter",  L"Dashed Line",  L"Arrow",         L"Rectangle",   L"Rounded Box",  L"Circle",
        L"Marker Circle",L"Bold Arrow",   L"Click Hand",    L"Spotlight",   L"Mosaic Blur",  L"Magnifier",
        L"Camera Zoom"
    };
    if (cardIndex < 0 || cardIndex >= 19) return isZh ? L"标注" : L"Mark";
    return isZh ? kZh[cardIndex] : kEn[cardIndex];
}

} // namespace

StudioPreviewWindow& StudioPreviewWindow::instance() {
    static StudioPreviewWindow inst;
    return inst;
}

bool StudioPreviewWindow::isVisible() const noexcept {
    return m_hwnd && IsWindow(m_hwnd) && IsWindowVisible(m_hwnd);
}

void StudioPreviewWindow::reloadTheme() {
    if (!m_hwnd || !IsWindow(m_hwnd)) return;
    const std::string themePref = tools3000::core::ConfigManager::instance().get<std::string>("/general/theme", "system");
    bool isDark = false;
    if (themePref == "dark") isDark = true;
    else if (themePref == "light") isDark = false;
    else isDark = tools3000::core::WinUtils::isSystemDarkMode();
    const BOOL darkFlag = isDark ? TRUE : FALSE;
    DwmSetWindowAttribute(m_hwnd, 20 /*DWMWA_USE_IMMERSIVE_DARK_MODE*/, &darkFlag, sizeof(darkFlag));
    if (IsWindowVisible(m_hwnd)) {
        InvalidateRect(m_hwnd, nullptr, FALSE);
    }
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
    clampTimelineViewWindow(keptSec);
    if (m_selectedClipIdx >= static_cast<int>(m_videoClips.size())) {
        m_selectedClipIdx = static_cast<int>(m_videoClips.size()) - 1;
    }
}

void StudioPreviewWindow::resequenceAutoStepBadges() {
    m_nextStepBadgeNumber = StudioTimelineAnalyzer::resequenceAutoStepBadges(
        m_timeline.annotations, m_videoClips, nullptr);
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
    resequenceAutoStepBadges();
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
    resequenceAutoStepBadges();
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
        resequenceAutoStepBadges();
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
        resequenceAutoStepBadges();
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

void StudioPreviewWindow::browseWatermarkLogoImageAsync() {
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
                    {isZh ? L"透明图片与 Logo (*.png;*.webp;*.jpg;*.jpeg;*.bmp)" : L"Logo & Images (*.png;*.webp;*.jpg;*.jpeg;*.bmp)",
                     L"*.png;*.webp;*.jpg;*.jpeg;*.bmp"},
                    {isZh ? L"所有文件 (*.*)" : L"All Files (*.*)", L"*.*"}
                };
                dlg->SetFileTypes(static_cast<UINT>(std::size(spec)), spec);
                dlg->SetTitle(isZh ? L"导入水印图片 / 企业 Logo" : L"Import Watermark Logo Image");
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
            m_pendingWatermarkImgWide = std::move(chosenPath);
        }
        m_fileDialogInFlight.store(false, std::memory_order_release);
        if (targetHwnd && IsWindow(targetHwnd)) {
            PostMessageW(targetHwnd, WM_STUDIO_WATERMARK_IMG_PICKED, 0, 0);
        }
    });
}

void StudioPreviewWindow::onWatermarkLogoPicked() {
    std::wstring picked;
    {
        std::lock_guard lock(m_fileDialogMutex);
        picked = std::move(m_pendingWatermarkImgWide);
        m_pendingWatermarkImgWide.clear();
    }
    joinFileDialogWorkerIfDone(false);
    if (picked.empty()) return;

    const std::string utf8 = tools3000::core::WinUtils::wstringToUtf8(picked);
    m_config.watermark.imagePathUtf8 = utf8;
    m_config.watermark.mode = WatermarkMode::ImageLogo;
    m_config.watermark.enabled = true;
    if (m_config.watermark.opacity < 0.35f) {
        m_config.watermark.opacity = 0.88f;
    }
    WatermarkRenderer::pushStudioRecentLogoPath(utf8);
    WatermarkRenderer::saveStudioWatermarkConfig(m_config.watermark);
    m_recentLogoPaths = WatermarkRenderer::loadStudioRecentLogoPaths();
    m_recentLogoThumbsDirty = true;

    updateCurrentComposedBitmap();
    if (m_hwnd) {
        SetForegroundWindow(m_hwnd);
        InvalidateRect(m_hwnd, nullptr, FALSE);
    }
}

void StudioPreviewWindow::applyWindowRoundingForCurrentState(int physW, int physH) {
    if (!m_hwnd || !IsWindow(m_hwnd) || physW <= 0 || physH <= 0) return;
    if (m_isMaximized || IsZoomed(m_hwnd)) {
        const DWM_WINDOW_CORNER_PREFERENCE flatCorner = DWMWCP_DONOTROUND;
        DwmSetWindowAttribute(m_hwnd, DWMWA_WINDOW_CORNER_PREFERENCE, &flatCorner, sizeof(flatCorner));
        SetWindowRgn(m_hwnd, nullptr, TRUE);
    } else {
        const DWM_WINDOW_CORNER_PREFERENCE roundCorner = DWMWCP_ROUND;
        const HRESULT hr = DwmSetWindowAttribute(
            m_hwnd, DWMWA_WINDOW_CORNER_PREFERENCE, &roundCorner, sizeof(roundCorner));
        if (SUCCEEDED(hr)) {
            // Windows 11 原生 DWM 硬件圆角已生效：清除 RGN 以完整保留原生高斯阴影、Aero Snap 动画与四角零死角拉伸热区
            SetWindowRgn(m_hwnd, nullptr, FALSE);
        } else {
            // Windows 10 / Server 2019/2022 / RDP 回退内核 RGN 圆角双保险
            tools3000::core::WinUtils::applyUniversalRoundedCorners(
                m_hwnd, physW, physH, tools3000::core::dpi::scaleMetric(12, m_dpiScale));
        }
    }
}

void StudioPreviewWindow::toggleMaximizeRestore() {
    if (!m_hwnd || !IsWindow(m_hwnd) || m_exportModalOpen) return;

    if (IsZoomed(m_hwnd)) {
        ShowWindow(m_hwnd, SW_RESTORE);
    } else {
        GetWindowRect(m_hwnd, &m_restoreWindowRect);
        ShowWindow(m_hwnd, SW_MAXIMIZE);
    }
    m_isMaximized = (IsZoomed(m_hwnd) != FALSE);
    tools3000::core::ConfigManager::instance().set<bool>("/recording/studioWindowMaximized", m_isMaximized);
    m_composedDirty = true;
    updateCurrentComposedBitmap();
    InvalidateRect(m_hwnd, nullptr, FALSE);
}

bool StudioPreviewWindow::ensureWindowCreated() {
    if (m_hwnd && IsWindow(m_hwnd)) return true;

    HINSTANCE hInst = GetModuleHandleW(nullptr);
    const HMONITOR mon = tools3000::core::dpi::activeMonitor();
    const unsigned dpi = tools3000::core::dpi::effectiveDpiForMonitor(mon);
    m_dpiScale = tools3000::core::dpi::scaleForDpi(dpi);

    HICON hBigIcon = static_cast<HICON>(LoadImageW(
        hInst, MAKEINTRESOURCEW(IDI_APP_ICON), IMAGE_ICON,
        GetSystemMetricsForDpi(SM_CXICON, dpi), GetSystemMetricsForDpi(SM_CYICON, dpi),
        LR_DEFAULTCOLOR | LR_SHARED));
    if (!hBigIcon) hBigIcon = LoadIconW(hInst, MAKEINTRESOURCEW(IDI_APP_ICON));
    HICON hSmallIcon = static_cast<HICON>(LoadImageW(
        hInst, MAKEINTRESOURCEW(IDI_APP_ICON), IMAGE_ICON,
        GetSystemMetricsForDpi(SM_CXSMICON, dpi), GetSystemMetricsForDpi(SM_CYSMICON, dpi),
        LR_DEFAULTCOLOR | LR_SHARED));

    static bool s_registered = false;
    if (!s_registered) {
        WNDCLASSEXW wc{};
        wc.cbSize = sizeof(wc);
        wc.style = CS_HREDRAW | CS_VREDRAW | CS_DBLCLKS;
        wc.lpfnWndProc = &StudioPreviewWindow::wndProc;
        wc.hInstance = hInst;
        wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        wc.hIcon = hBigIcon;
        wc.hIconSm = hSmallIcon;
        wc.lpszClassName = kStudioWndClass;
        if (RegisterClassExW(&wc) || GetLastError() == ERROR_CLASS_ALREADY_EXISTS) {
            s_registered = true;
        }
    }

    auto& cm = tools3000::core::ConfigManager::instance();
    m_logicalWidth = std::clamp(cm.get<int>("/recording/studioWindowWidth", 1260), 1020, 3840);
    m_logicalHeight = std::clamp(cm.get<int>("/recording/studioWindowHeight", 776), 650, 2160);

    const RECT work = tools3000::core::dpi::workArea(mon);
    const SIZE desiredPhys{
        tools3000::core::dpi::scaleMetric(m_logicalWidth, m_dpiScale),
        tools3000::core::dpi::scaleMetric(m_logicalHeight, m_dpiScale)
    };
    const SIZE fitted = tools3000::core::dpi::fitSizeToWorkArea(
        desiredPhys, work, tools3000::core::dpi::scaleMetric(20, m_dpiScale));
    const int posX = work.left + ((work.right - work.left) - fitted.cx) / 2;
    const int posY = work.top + ((work.bottom - work.top) - fitted.cy) / 2;

    // 采用标准 WS_OVERLAPPEDWINDOW 顶层窗口架构（包含 WS_THICKFRAME / WS_MAXIMIZEBOX / WS_MINIMIZEBOX），
    // 原生支持四边与四角自由拖拽缩放、Aero Snap 边缘贴靠、双击最大化与 Win+方向键分屏
    m_hwnd = CreateWindowExW(
        WS_EX_APPWINDOW,
        kStudioWndClass,
        L"Tools3000 Studio",
        WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN,
        posX, posY, fitted.cx, fitted.cy,
        nullptr, nullptr, hInst, this);
    if (!m_hwnd) {
        LOG_ERROR("StudioPreviewWindow: CreateWindowExW failed, err={}", GetLastError());
        return false;
    }

    MARGINS margins = {1, 1, 1, 1};
    DwmExtendFrameIntoClientArea(m_hwnd, &margins);
    SetWindowPos(m_hwnd, nullptr, posX, posY, fitted.cx, fitted.cy,
                 SWP_NOZORDER | SWP_NOACTIVATE | SWP_FRAMECHANGED);

    if (hBigIcon) SendMessageW(m_hwnd, WM_SETICON, ICON_BIG, reinterpret_cast<LPARAM>(hBigIcon));
    if (hSmallIcon) SendMessageW(m_hwnd, WM_SETICON, ICON_SMALL, reinterpret_cast<LPARAM>(hSmallIcon));
    tools3000::core::WinUtils::setWindowAppUserModelId(m_hwnd, L"Yy1.Tools3000");

    m_restoreWindowRect = {posX, posY, posX + fitted.cx, posY + fitted.cy};
    tools3000::core::WinUtils::allowWindowCapture(m_hwnd);
    applyWindowRoundingForCurrentState(fitted.cx, fitted.cy);
    return true;
}

double StudioPreviewWindow::computeEffectiveDurationSec() const {
    if (m_videoClips.empty()) return m_durationSec;
    const int64_t keptMs = StudioTimelineAnalyzer::computeClipsKeptDurationMs(m_videoClips);
    return (std::max)(0.2, static_cast<double>(keptMs) / 1000.0);
}

double StudioPreviewWindow::visibleTimelineSpanSec(double keptDurSec) const noexcept {
    const double safeDur = (std::max)(0.2, keptDurSec);
    const double z = std::clamp(static_cast<double>(m_timelineZoom), 1.0, 24.0);
    return (std::max)(0.25, safeDur / z);
}

void StudioPreviewWindow::clampTimelineViewWindow(double keptDurSec) noexcept {
    const double safeDur = (std::max)(0.2, keptDurSec);
    m_timelineZoom = std::clamp(m_timelineZoom, 1.0f, 24.0f);
    if (m_timelineZoom <= 1.005f) {
        m_timelineZoom = 1.0f;
        m_timelineViewStartSec = 0.0;
        return;
    }
    const double span = visibleTimelineSpanSec(safeDur);
    m_timelineViewStartSec = std::clamp(m_timelineViewStartSec, 0.0, (std::max)(0.0, safeDur - span));
}

void StudioPreviewWindow::setTimelineZoomAnchored(
    float newZoom, double anchorKeptSec, float anchorNorm01, double keptDurSec) noexcept {
    const double safeDur = (std::max)(0.2, keptDurSec);
    m_timelineZoom = std::clamp(newZoom, 1.0f, 24.0f);
    if (m_timelineZoom <= 1.015f) {
        m_timelineZoom = 1.0f;
        m_timelineViewStartSec = 0.0;
        return;
    }
    const double newSpan = visibleTimelineSpanSec(safeDur);
    const double clampedAnchor = std::clamp(anchorKeptSec, 0.0, safeDur);
    const double u = std::clamp(static_cast<double>(anchorNorm01), 0.0, 1.0);
    m_timelineViewStartSec = std::clamp(clampedAnchor - u * newSpan, 0.0, (std::max)(0.0, safeDur - newSpan));
}

float StudioPreviewWindow::mapKeptSecToTrackX(
    double keptSec, float laneLeft, float trackW, double keptDurSec) const noexcept {
    const double span = visibleTimelineSpanSec(keptDurSec);
    const double rel = (keptSec - m_timelineViewStartSec) / (std::max)(1e-5, span);
    return laneLeft + static_cast<float>(rel) * (std::max)(1.0f, trackW);
}

double StudioPreviewWindow::mapTrackXToKeptSec(
    float lx, float laneLeft, float trackW, double keptDurSec) const noexcept {
    const double safeDur = (std::max)(0.2, keptDurSec);
    const double span = visibleTimelineSpanSec(safeDur);
    const double u = std::clamp(static_cast<double>((lx - laneLeft) / (std::max)(1.0f, trackW)), 0.0, 1.0);
    return std::clamp(m_timelineViewStartSec + u * span, 0.0, safeDur);
}

void StudioPreviewWindow::initExportModalDefaults() {
    m_exportModalOpen = false;
    m_exportCompletedCard = false;
    m_editingExportName = false;
    m_userEditedExportStem = false;
    m_openDropdown = ExportDropdownKind::None;
    m_sessionExportedPathsLower.clear();
    m_lastExportedFilePath.clear();
    m_lastExportedFileBytes = 0;
    m_lastExportedSpecLine.clear();
    m_copyFeedbackUntilTick = 0;

    const std::filesystem::path rawFsPath(tools3000::core::WinUtils::utf8ToWstring(m_rawVideoPath));
    m_exportFileNameStem = rawFsPath.stem().wstring();
    if (m_exportFileNameStem.empty()) {
        m_exportFileNameStem = L"Tools3000_Studio";
    }
    m_initialFileNameStem = m_exportFileNameStem;
    const std::wstring rawParent = rawFsPath.parent_path().wstring();

    auto& cm = tools3000::core::ConfigManager::instance();
    m_saveExportPrefs = cm.get<bool>("/recording/studioExport/savePrefs", true);
    m_autoCloseAfterExport = cm.get<bool>("/recording/studioExport/autoClose", false);
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
    ensureNonCollidingExportStem();
}

static std::wstring normalizeLowerFsKey(const std::filesystem::path& p) {
    std::error_code ec;
    std::wstring s = std::filesystem::weakly_canonical(p, ec).wstring();
    if (ec || s.empty()) {
        s = p.lexically_normal().wstring();
    }
    std::transform(s.begin(), s.end(), s.begin(), [](wchar_t c) {
        return static_cast<wchar_t>(std::towlower(c));
    });
    return s;
}

void StudioPreviewWindow::ensureNonCollidingExportStem() {
    if (m_userEditedExportStem || m_sessionExportedPathsLower.empty()) {
        return;
    }
    const std::wstring baseStem = m_initialFileNameStem.empty() ? L"Tools3000_Studio" : m_initialFileNameStem;
    const std::wstring extW = tools3000::core::WinUtils::utf8ToWstring(
        StudioVideoCompositor::extensionForExportFormat(m_exportOpts.format));
    const std::filesystem::path dirPath = m_exportDirectoryWide.empty()
        ? std::filesystem::path(tools3000::core::WinUtils::utf8ToWstring(m_rawVideoPath)).parent_path()
        : std::filesystem::path(m_exportDirectoryWide);

    auto wasAlreadyExportedInSession = [&](const std::wstring& candidateStem) -> bool {
        const std::wstring key = normalizeLowerFsKey(dirPath / (candidateStem + extW));
        return std::find(m_sessionExportedPathsLower.begin(), m_sessionExportedPathsLower.end(), key) !=
               m_sessionExportedPathsLower.end();
    };

    if (!wasAlreadyExportedInSession(baseStem)) {
        m_exportFileNameStem = baseStem;
        return;
    }
    for (int suffix = 2; suffix <= 99; ++suffix) {
        const std::wstring cand = baseStem + L"_" + std::to_wstring(suffix);
        if (!wasAlreadyExportedInSession(cand)) {
            m_exportFileNameStem = cand;
            return;
        }
    }
}

void StudioPreviewWindow::persistExportPreferencesIfChecked() {
    auto& cm = tools3000::core::ConfigManager::instance();
    cm.set<bool>("/recording/studioExport/savePrefs", m_saveExportPrefs);
    cm.set<bool>("/recording/studioExport/autoClose", m_autoCloseAfterExport);
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

bool StudioPreviewWindow::showPreparingOverlay(double recordedDurationSec) {
    joinExportWorkerIfDone(true);
    if (!ensureWindowCreated()) {
        return false;
    }

    m_preparingSession = true;
    m_prepareStartTick = GetTickCount64();
    m_prepareRecordedDurSec = (std::max)(0.0, recordedDurationSec);
    m_playing = false;
    m_hoverSkimming = false;
    m_exportModalOpen = false;
    m_exportCompletedCard = false;
    m_lastTickMs = GetTickCount();

    auto& cm = tools3000::core::ConfigManager::instance();
    m_logicalWidth = std::clamp(cm.get<int>("/recording/studioWindowWidth", 1260), 1020, 3840);
    m_logicalHeight = std::clamp(cm.get<int>("/recording/studioWindowHeight", 776), 650, 2160);
    m_isMaximized = cm.get<bool>("/recording/studioWindowMaximized", false);

    const HMONITOR mon = tools3000::core::dpi::activeMonitor();
    m_dpiScale = tools3000::core::dpi::scaleForMonitor(mon);
    const RECT work = tools3000::core::dpi::workArea(mon);
    const SIZE desiredPhys{
        tools3000::core::dpi::scaleMetric(m_logicalWidth, m_dpiScale),
        tools3000::core::dpi::scaleMetric(m_logicalHeight, m_dpiScale)
    };
    const SIZE fitted = tools3000::core::dpi::fitSizeToWorkArea(
        desiredPhys, work, tools3000::core::dpi::scaleMetric(20, m_dpiScale));
    const int normX = work.left + ((work.right - work.left) - fitted.cx) / 2;
    const int normY = work.top + ((work.bottom - work.top) - fitted.cy) / 2;
    m_restoreWindowRect = {normX, normY, normX + fitted.cx, normY + fitted.cy};

    const int initWinW = m_isMaximized ? (work.right - work.left) : fitted.cx;
    const int initWinH = m_isMaximized ? (work.bottom - work.top) : fitted.cy;

    if (IsIconic(m_hwnd)) {
        ShowWindow(m_hwnd, SW_RESTORE);
    }
    if (IsZoomed(m_hwnd) && !m_isMaximized) {
        ShowWindow(m_hwnd, SW_RESTORE);
    }
    SetWindowPos(m_hwnd, HWND_TOPMOST, normX, normY, fitted.cx, fitted.cy,
                 m_isMaximized ? SWP_NOACTIVATE : SWP_SHOWWINDOW);
    if (m_isMaximized) {
        ShowWindow(m_hwnd, SW_MAXIMIZE);
    } else {
        ShowWindow(m_hwnd, SW_SHOW);
    }
    applyWindowRoundingForCurrentState(initWinW, initWinH);

    AllowSetForegroundWindow(ASFW_ANY);
    const HWND fgHwnd = GetForegroundWindow();
    const DWORD curTid = GetCurrentThreadId();
    const DWORD fgTid = fgHwnd ? GetWindowThreadProcessId(fgHwnd, nullptr) : 0;
    bool attached = false;
    if (fgHwnd && fgHwnd != m_hwnd && fgTid != 0 && fgTid != curTid) {
        attached = (AttachThreadInput(curTid, fgTid, TRUE) != FALSE);
    }
    BringWindowToTop(m_hwnd);
    SetForegroundWindow(m_hwnd);
    SetActiveWindow(m_hwnd);
    SetFocus(m_hwnd);
    if (attached) {
        AttachThreadInput(curTid, fgTid, FALSE);
    }
    SetWindowPos(m_hwnd, HWND_NOTOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_SHOWWINDOW);

    SetTimer(m_hwnd, kPlaybackTimerId, kTimerIntervalMs, nullptr);
    const bool isZhTitle = tools3000::core::NativeThemePalette::isUiLanguageChinese();
    SetWindowTextW(m_hwnd, isZhTitle ? L"Tools3000 录屏演播室 - 正在准备..." : L"Tools3000 Studio - Preparing...");
    RedrawWindow(m_hwnd, nullptr, nullptr, RDW_INVALIDATE | RDW_UPDATENOW);
    return true;
}

void StudioPreviewWindow::dismissPreparingOverlay() {
    if (!m_preparingSession) return;
    m_preparingSession = false;
    closeSessionAndCleanup();
}

bool StudioPreviewWindow::showSession(
    const std::string& rawVideoPathUtf8,
    StudioTimelineData timeline,
    const StudioRenderConfig& initialConfig,
    int bitrateMbps,
    CompletionHandler onFinished,
    std::vector<cv::Mat> preloadedDeck) {
    if (rawVideoPathUtf8.empty()) {
        dismissPreparingOverlay();
        return false;
    }

    const bool wasAlreadyShowingPrepare = (m_preparingSession && m_hwnd && IsWindowVisible(m_hwnd));
    joinExportWorkerIfDone(true);
    joinFileDialogWorkerIfDone(true);

    std::vector<cv::Mat> deck = std::move(preloadedDeck);
    int origW = timeline.sourceWidth;
    int origH = timeline.sourceHeight;
    double durSec = (timeline.durationMs > 0) ? (timeline.durationMs / 1000.0) : 0.0;

    if (deck.empty()) {
        if (!StudioVideoCompositor::extractPreviewDeck(rawVideoPathUtf8, 1280, 224, deck, origW, origH, durSec) ||
            deck.empty()) {
            LOG_WARN("StudioPreviewWindow: failed to extract preview deck from {}, falling back to direct raw completion",
                     rawVideoPathUtf8);
            dismissPreparingOverlay();
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
        m_preparingSession = false;
        if (onFinished) {
            onFinished(rawVideoPathUtf8, durSec, false);
        }
        return false;
    }

    m_preparingSession = false;
    m_rawVideoPath = rawVideoPathUtf8;
    m_masterVideoPath = rawVideoPathUtf8;
    m_masterIsIsolatedTemp = false;
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
    m_filmstripDirty = true;
    m_origWidth = origW;
    m_origHeight = origH;

    const int64_t rawTotalMs = (std::max<int64_t>)(100, static_cast<int64_t>(std::llround(m_durationSec * 1000.0)));
    if (m_timeline.audioWaveformPeaks.empty()) {
        StudioVideoCompositor::extractAudioWaveformPeaks(m_rawVideoPath, 4096, m_timeline.audioWaveformPeaks);
    }
    m_waveformProfile = StudioTimelineAnalyzer::analyzeWaveformProfile(m_timeline.audioWaveformPeaks);
    m_hasActiveAudioWaveform = m_waveformProfile.hasAudio;
    m_waveformReferencePeak = m_waveformProfile.referencePeak;
    m_waveformNoiseFloor = m_waveformProfile.noiseFloor;

    m_videoClips = StudioTimelineAnalyzer::buildClipsFromCutSegments(rawTotalMs, m_timeline.cutSegments);
    m_undoStack.clear();
    m_redoStack.clear();

    m_activeTab = LeftRailTab::CanvasBg;
    m_nextStepBadgeNumber = 1;
    m_editingAnnotationText = false;
    m_editingWatermarkText = false;
    m_watermarkStageDragMode = 0;
    m_hoveredWatermarkOnStage = false;
    m_hoveredWatermarkCorner = false;
    m_lastWatermarkStageRect = {};
    m_config.watermark = WatermarkRenderer::loadStudioWatermarkConfig();
    m_recentLogoPaths = WatermarkRenderer::loadStudioRecentLogoPaths();
    m_recentLogoThumbsDirty = true;
    m_stageAnnDrag = StageAnnDragKind::None;
    m_hoveredStageAnnIdx = -1;
    m_hoveredStageAnnPart = StageAnnDragKind::None;
    m_draggingZoomFocus = false;
    m_hoveredZoomFocusReticle = false;
    m_pressedPresetCardIdx = -1;
    m_draggingPresetToStage = false;
    m_selectedClipIdx = -1;
    m_selectedZoomIdx = -1;
    m_selectedAnnIdx = -1;
    m_trackDragKind = TrackDragKind::None;
    m_activeSlider = SliderDragKind::None;
    m_timelineZoom = 1.0f;
    m_timelineViewStartSec = 0.0;
    m_clickDemoStartTick = 0;
    m_annDemoStartTick = 0;
    m_hoveredCoachId = 0;
    m_coachHoverEnterTick = 0;
    m_coachLastActiveTick = 0;
    resequenceAutoStepBadges();

    discoverLocalWallpapers();
    initExportModalDefaults();

    m_exporting.store(false, std::memory_order_release);
    m_cancelExport.store(false, std::memory_order_release);
    m_exportProgress.store(0.0, std::memory_order_release);

    m_playheadSec = 0.0;
    m_timelineZoom = 1.0f;
    m_timelineViewStartSec = 0.0;
    m_hoverSkimming = false;
    m_hoverSkimSec = 0.0;
    m_pendingSkimCompose = false;
    m_inInteractiveSizing = false;
    m_lastComposedUsedProxy = false;
    m_lastFastScrubTick = 0;
    m_playing = true;
    m_scrubbingTimeline = false;
    m_lastTickMs = GetTickCount();
    m_lastComposeQpcTick = queryQpcNow();

    if (wasAlreadyShowingPrepare) {
        RECT cli{};
        GetClientRect(m_hwnd, &cli);
        const int curWinW = (std::max)(640, static_cast<int>(cli.right - cli.left));
        const int curWinH = (std::max)(480, static_cast<int>(cli.bottom - cli.top));
        m_stageViewportPhysW = (std::max)(640, curWinW - tools3000::core::dpi::scaleMetric(400, m_dpiScale));
        m_stageViewportPhysH = (std::max)(360, curWinH - tools3000::core::dpi::scaleMetric(256, m_dpiScale));
    } else {
        auto& cm = tools3000::core::ConfigManager::instance();
        m_logicalWidth = std::clamp(cm.get<int>("/recording/studioWindowWidth", 1260), 1020, 3840);
        m_logicalHeight = std::clamp(cm.get<int>("/recording/studioWindowHeight", 776), 650, 2160);
        m_isMaximized = cm.get<bool>("/recording/studioWindowMaximized", false);

        const HMONITOR mon = tools3000::core::dpi::activeMonitor();
        m_dpiScale = tools3000::core::dpi::scaleForMonitor(mon);
        const RECT work = tools3000::core::dpi::workArea(mon);
        const SIZE desiredPhys{
            tools3000::core::dpi::scaleMetric(m_logicalWidth, m_dpiScale),
            tools3000::core::dpi::scaleMetric(m_logicalHeight, m_dpiScale)
        };
        const SIZE fitted = tools3000::core::dpi::fitSizeToWorkArea(
            desiredPhys, work, tools3000::core::dpi::scaleMetric(20, m_dpiScale));
        const int normX = work.left + ((work.right - work.left) - fitted.cx) / 2;
        const int normY = work.top + ((work.bottom - work.top) - fitted.cy) / 2;
        m_restoreWindowRect = {normX, normY, normX + fitted.cx, normY + fitted.cy};

        const int initWinW = m_isMaximized ? (work.right - work.left) : fitted.cx;
        const int initWinH = m_isMaximized ? (work.bottom - work.top) : fitted.cy;
        m_stageViewportPhysW = (std::max)(640, initWinW - tools3000::core::dpi::scaleMetric(400, m_dpiScale));
        m_stageViewportPhysH = (std::max)(360, initWinH - tools3000::core::dpi::scaleMetric(256, m_dpiScale));

        if (IsIconic(m_hwnd)) {
            ShowWindow(m_hwnd, SW_RESTORE);
        }
        if (IsZoomed(m_hwnd) && !m_isMaximized) {
            ShowWindow(m_hwnd, SW_RESTORE);
        }
        SetWindowPos(m_hwnd, HWND_TOPMOST, normX, normY, fitted.cx, fitted.cy,
                     m_isMaximized ? SWP_NOACTIVATE : SWP_SHOWWINDOW);
        if (m_isMaximized) {
            ShowWindow(m_hwnd, SW_MAXIMIZE);
        } else {
            ShowWindow(m_hwnd, SW_SHOW);
        }
        applyWindowRoundingForCurrentState(initWinW, initWinH);

        AllowSetForegroundWindow(ASFW_ANY);
        const HWND fgHwnd = GetForegroundWindow();
        const DWORD curTid = GetCurrentThreadId();
        const DWORD fgTid = fgHwnd ? GetWindowThreadProcessId(fgHwnd, nullptr) : 0;
        bool attached = false;
        if (fgHwnd && fgHwnd != m_hwnd && fgTid != 0 && fgTid != curTid) {
            attached = (AttachThreadInput(curTid, fgTid, TRUE) != FALSE);
        }
        BringWindowToTop(m_hwnd);
        SetForegroundWindow(m_hwnd);
        SetActiveWindow(m_hwnd);
        SetFocus(m_hwnd);
        if (attached) {
            AttachThreadInput(curTid, fgTid, FALSE);
        }
        SetWindowPos(m_hwnd, HWND_NOTOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_SHOWWINDOW);
    }

    m_hiResReader.open(m_masterVideoPath, 3200);
    rebakeTimelinePreview();
    updateCurrentComposedBitmap();

    SetTimer(m_hwnd, kPlaybackTimerId, kTimerIntervalMs, nullptr);
    const bool isZhReady = tools3000::core::NativeThemePalette::isUiLanguageChinese();
    SetWindowTextW(m_hwnd, isZhReady ? L"Tools3000 录屏演播室" : L"Tools3000 Studio");
    InvalidateRect(m_hwnd, nullptr, FALSE);

    LOG_INFO("StudioPreviewWindow opened: raw={}, deckFrames={}, zoomSegments={}, wallpapers={}",
             m_rawVideoPath, m_previewDeckBgr.size(),
             m_timeline.zoomSegments.size(), m_allWallpapers.size());
    return true;
}

void StudioPreviewWindow::rebakeTimelinePreview() {
    // 3~4 小时超长录屏自适应关键帧烘焙：<= 40 分钟保持 30Hz 满帧率；数小时超长片平滑降频封顶 <= 72,000 帧并辅以帧间线性插值
    constexpr double kMaxBakedFrames = 72000.0;
    const int adaptiveFps = (m_durationSec > (kMaxBakedFrames / 30.0))
        ? std::clamp(static_cast<int>(std::floor(kMaxBakedFrames / (std::max)(1.0, m_durationSec))), 4, 30)
        : 30;
    m_previewEvalFps = adaptiveFps;
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

    StudioEvaluatedFrame eval{};
    if (!m_evaluatedFrames.empty()) {
        const double exactF = std::clamp(
            srcSec * static_cast<double>(m_previewEvalFps),
            0.0,
            static_cast<double>(m_evaluatedFrames.size() - 1));
        const size_t i0 = static_cast<size_t>(std::floor(exactF));
        const size_t i1 = (std::min)(m_evaluatedFrames.size() - 1, i0 + 1);
        const float frac = static_cast<float>(exactF - static_cast<double>(i0));
        eval = m_evaluatedFrames[i0];
        if (i1 > i0 && frac > 1e-4f) {
            const auto& e1 = m_evaluatedFrames[i1];
            auto lerpF = [frac](float a, float b) { return a + (b - a) * frac; };
            eval.camera.centerX = lerpF(eval.camera.centerX, e1.camera.centerX);
            eval.camera.centerY = lerpF(eval.camera.centerY, e1.camera.centerY);
            eval.camera.zoom = lerpF(eval.camera.zoom, e1.camera.zoom);
            eval.camera.cropX = lerpF(eval.camera.cropX, e1.camera.cropX);
            eval.camera.cropY = lerpF(eval.camera.cropY, e1.camera.cropY);
            eval.camera.cropW = lerpF(eval.camera.cropW, e1.camera.cropW);
            eval.camera.cropH = lerpF(eval.camera.cropH, e1.camera.cropH);
            eval.camera.tiltYawDeg = lerpF(eval.camera.tiltYawDeg, e1.camera.tiltYawDeg);
            eval.camera.tiltPitchDeg = lerpF(eval.camera.tiltPitchDeg, e1.camera.tiltPitchDeg);
            eval.smoothCursorX = lerpF(eval.smoothCursorX, e1.smoothCursorX);
            eval.smoothCursorY = lerpF(eval.smoothCursorY, e1.smoothCursorY);
        }
    }

    const cv::Mat& proxyFrame = m_previewDeckBgr[deckIdx];
    const bool fastTimeScrub = m_hoverSkimming || m_scrubbingTimeline || m_trackDragKind != TrackDragKind::None;
    const bool interactiveControlDrag =
        m_draggingZoomFocus ||
        (m_activeSlider != SliderDragKind::None) ||
        (m_stageAnnDrag != StageAnnDragKind::None) ||
        (m_watermarkStageDragMode != 0);
    cv::Mat nativeFrame;
    bool useNative = false;
    if (m_hiResReader.isOpen()) {
        if (fastTimeScrub) {
            m_lastFastScrubTick = GetTickCount64();
            useNative = m_hiResReader.tryGetCachedFrame(srcSec, nativeFrame, 0.020);
        } else if (!m_playing) {
            useNative = m_hiResReader.decodeFrameAt(srcSec, nativeFrame);
        } else {
            const bool needHiResWhilePlaying =
                (m_stageViewportPhysW > proxyFrame.cols + 24) ||
                (eval.camera.zoom > 1.08f) ||
                (m_hiResReader.nativeWidth() > proxyFrame.cols);
            if (needHiResWhilePlaying) {
                useNative = m_hiResReader.decodeFrameAt(srcSec, nativeFrame);
            }
        }
    }
    m_lastComposedUsedProxy = (!useNative && m_hiResReader.isOpen()) || interactiveControlDrag;
    const cv::Mat& srcFrame = (useNative && !nativeFrame.empty()) ? nativeFrame : proxyFrame;

    // 交互滑块/标注/水印连续拖拽期间采用 1360px 瞬态高速画布保障 120FPS 丝滑跟手，松手瞬间自动升格全清视网膜画布
    const int refOrigW = (std::max)(1, m_origWidth > 0 ? m_origWidth : srcFrame.cols);
    const int refOrigH = (std::max)(1, m_origHeight > 0 ? m_origHeight : srcFrame.rows);
    const double srcAspect = static_cast<double>(refOrigW) / static_cast<double>(refOrigH);
    const int vpFitW = (std::max)(
        960,
        (std::min)(
            m_stageViewportPhysW,
            static_cast<int>(std::round(static_cast<double>(m_stageViewportPhysH) * srcAspect))));
    const int maxCanvasW = (fastTimeScrub || interactiveControlDrag)
        ? 1360
        : std::clamp((std::max)(refOrigW, m_stageViewportPhysW), 1280, 3200);
    const int desiredBaseW = std::clamp(vpFitW, 960, maxCanvasW) & ~1;
    const int desiredBaseH = (std::max)(360, static_cast<int>(std::round(desiredBaseW / srcAspect)) & ~1);
    const cv::Size targetSz = (m_config.aspectRatio == StudioAspectRatio::Original)
        ? cv::Size(desiredBaseW, desiredBaseH)
        : StudioVideoCompositor::resolveOutputCanvasSize(desiredBaseW, desiredBaseH, m_config.aspectRatio);

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
    StudioEvaluatedFrame activeEval = eval;

    // 拖拽运镜焦点时，展开为 100% 全景光学取景框原片视野，彻底消除相机中心自激正反馈与裁切盲区！
    if (m_draggingZoomFocus) {
        const float fullSrcW = static_cast<float>((std::max)(1, m_timeline.sourceWidth > 0 ? m_timeline.sourceWidth : refOrigW));
        const float fullSrcH = static_cast<float>((std::max)(1, m_timeline.sourceHeight > 0 ? m_timeline.sourceHeight : refOrigH));
        activeEval.camera.centerX = fullSrcW * 0.5f;
        activeEval.camera.centerY = fullSrcH * 0.5f;
        activeEval.camera.zoom = 1.0f;
        activeEval.camera.cropX = 0.0f;
        activeEval.camera.cropY = 0.0f;
        activeEval.camera.cropW = fullSrcW;
        activeEval.camera.cropH = fullSrcH;
        activeEval.camera.tiltYawDeg = 0.0f;
        activeEval.camera.tiltPitchDeg = 0.0f;
        activeEval.cameraVelocityPx = 0.0f;
    }
    m_lastEvalCamera = activeEval.camera;
    if (m_clickDemoStartTick > 0) {
        const ULONGLONG now64 = GetTickCount64();
        const int64_t demoElapsed = (now64 >= m_clickDemoStartTick)
            ? static_cast<int64_t>(now64 - m_clickDemoStartTick)
            : 0;
        const int64_t demoWaveMs =
            (m_config.clickEffectStyle == StudioClickEffectStyle::DualRipple) ? 430
            : ((m_config.clickEffectStyle == StudioClickEffectStyle::SoftTouch) ? 290 : 310);
        const int64_t maxDemoMs = (std::max<int64_t>)(260, demoWaveMs);
        if (demoElapsed <= maxDemoMs) {
            if (activeEval.cursorKind == StudioCursorKind::Hidden) {
                activeEval.cursorKind = StudioCursorKind::Arrow;
                activeEval.smoothCursorX = activeEval.camera.centerX;
                activeEval.smoothCursorY = activeEval.camera.centerY;
            }
            if (demoElapsed <= 260) {
                const float u = static_cast<float>(demoElapsed) / 260.0f;
                if (u <= 0.24f) {
                    const float k = u / 0.24f;
                    const float inv = 1.0f - k;
                    const float easeDown = 1.0f - inv * inv * inv;
                    activeEval.clickScale = 1.0f - 0.21f * easeDown;
                    activeEval.clickTiltDeg = -3.4f * easeDown;
                } else {
                    const float k = (u - 0.24f) / 0.76f;
                    activeEval.clickScale = 1.0f - 0.21f * std::exp(-5.2f * k) * std::cos(4.4f * k);
                    activeEval.clickTiltDeg = -3.4f * std::exp(-6.0f * k) * std::cos(3.2f * k);
                }
            }
            if (demoElapsed <= demoWaveMs &&
                m_config.showClickRipple &&
                m_config.clickEffectStyle != StudioClickEffectStyle::TactileOnly) {
                activeEval.rippleProgress = static_cast<float>(demoElapsed) / static_cast<float>(demoWaveMs);
                activeEval.rippleSrcX = activeEval.smoothCursorX;
                activeEval.rippleSrcY = activeEval.smoothCursorY;
                activeEval.rippleIsRight = false;
            }
        }
    }
    int64_t annPreviewElapsedMs = -1;
    if (m_annDemoStartTick > 0 && m_selectedAnnIdx >= 0 &&
        m_selectedAnnIdx < static_cast<int>(m_timeline.annotations.size())) {
        const ULONGLONG now64 = GetTickCount64();
        if (now64 >= m_annDemoStartTick && (now64 - m_annDemoStartTick) <= 1450) {
            annPreviewElapsedMs = static_cast<int64_t>(now64 - m_annDemoStartTick);
        }
    }
    // 播放或掠览时全速运行动态标注三段式动力学；暂停静止编辑时展示 100% 闭合稳态以便像素级拖拽定位
    const bool animateAnns = m_playing || m_hoverSkimming || m_scrubbingTimeline || (annPreviewElapsedMs >= 0);
    cv::Mat composedBgr = m_previewCompositor.composeFrame(
        srcFrame, activeEval, m_config, targetSz, annPtr,
        animateAnns, annPreviewElapsedMs, m_selectedAnnIdx);
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
            uiFont, nullptr, DWRITE_FONT_WEIGHT_MEDIUM,
            DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, 11.9f, L"zh-CN", m_captionWrapFmt.GetAddressOf());
        m_dwriteFactory->CreateTextFormat(
            monoFont, nullptr, DWRITE_FONT_WEIGHT_SEMI_BOLD,
            DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, 11.9f, L"en-US", m_monoFmt.GetAddressOf());

        auto configureSingleLineEllipsis = [&](IDWriteTextFormat* fmt) {
            if (!fmt) return;
            fmt->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
            ComPtr<IDWriteInlineObject> ellipsis;
            if (SUCCEEDED(m_dwriteFactory->CreateEllipsisTrimmingSign(fmt, ellipsis.GetAddressOf())) && ellipsis) {
                DWRITE_TRIMMING trim{DWRITE_TRIMMING_GRANULARITY_CHARACTER, 0, 0};
                fmt->SetTrimming(&trim, ellipsis.Get());
            }
        };
        configureSingleLineEllipsis(m_titleFmt.Get());
        configureSingleLineEllipsis(m_sectionFmt.Get());
        configureSingleLineEllipsis(m_bodyFmt.Get());
        configureSingleLineEllipsis(m_captionFmt.Get());
        configureSingleLineEllipsis(m_monoFmt.Get());

        if (m_captionWrapFmt) {
            m_captionWrapFmt->SetWordWrapping(DWRITE_WORD_WRAPPING_WRAP);
            m_captionWrapFmt->SetLineSpacing(DWRITE_LINE_SPACING_METHOD_UNIFORM, 17.2f, 13.8f);
            ComPtr<IDWriteInlineObject> wrapEllipsis;
            if (SUCCEEDED(m_dwriteFactory->CreateEllipsisTrimmingSign(m_captionWrapFmt.Get(), wrapEllipsis.GetAddressOf())) && wrapEllipsis) {
                DWRITE_TRIMMING trim{DWRITE_TRIMMING_GRANULARITY_CHARACTER, 0, 0};
                m_captionWrapFmt->SetTrimming(&trim, wrapEllipsis.Get());
            }
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

    if (m_recentLogoThumbsDirty && m_renderTarget) {
        D2D1_BITMAP_PROPERTIES bpAlpha = D2D1::BitmapProperties(
            D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED),
            96.0f, 96.0f);
        for (size_t i = 0; i < m_recentLogoThumbBitmaps.size(); ++i) {
            m_recentLogoThumbBitmaps[i].Reset();
            if (i >= m_recentLogoPaths.size()) continue;
            cv::Mat logo = WatermarkRenderer::loadCachedLogoBgra(m_recentLogoPaths[i]);
            if (logo.empty()) continue;
            constexpr int kThumbSz = 64;
            const float sc = (std::min)(
                static_cast<float>(kThumbSz - 8) / static_cast<float>((std::max)(1, logo.cols)),
                static_cast<float>(kThumbSz - 8) / static_cast<float>((std::max)(1, logo.rows)));
            const int tw = std::clamp(static_cast<int>(std::round(logo.cols * sc)), 4, kThumbSz);
            const int th = std::clamp(static_cast<int>(std::round(logo.rows * sc)), 4, kThumbSz);
            cv::Mat scaled;
            cv::resize(logo, scaled, cv::Size(tw, th), 0.0, 0.0, sc < 1.0f ? cv::INTER_AREA : cv::INTER_CUBIC);
            cv::Mat canvas(kThumbSz, kThumbSz, CV_8UC4, cv::Scalar(0, 0, 0, 0));
            const int ox = (kThumbSz - tw) / 2;
            const int oy = (kThumbSz - th) / 2;
            for (int y = 0; y < th; ++y) {
                const uint8_t* sRow = scaled.ptr<uint8_t>(y);
                uint8_t* dRow = canvas.ptr<uint8_t>(oy + y) + ox * 4;
                for (int x = 0; x < tw; ++x) {
                    const uint32_t a = sRow[x * 4 + 3];
                    dRow[x * 4 + 0] = static_cast<uint8_t>((sRow[x * 4 + 0] * a + 127u) / 255u);
                    dRow[x * 4 + 1] = static_cast<uint8_t>((sRow[x * 4 + 1] * a + 127u) / 255u);
                    dRow[x * 4 + 2] = static_cast<uint8_t>((sRow[x * 4 + 2] * a + 127u) / 255u);
                    dRow[x * 4 + 3] = static_cast<uint8_t>(a);
                }
            }
            m_renderTarget->CreateBitmap(
                D2D1::SizeU(kThumbSz, kThumbSz),
                canvas.data,
                static_cast<UINT32>(canvas.step[0]),
                &bpAlpha,
                m_recentLogoThumbBitmaps[i].GetAddressOf());
        }
        m_recentLogoThumbsDirty = false;
    }

    if (m_filmstripDirty && m_renderTarget) {
        m_filmstripBitmaps.clear();
        if (!m_previewDeckBgr.empty()) {
            D2D1_BITMAP_PROPERTIES bp = D2D1::BitmapProperties(
                D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_IGNORE),
                96.0f, 96.0f);
            const size_t totalDeck = m_previewDeckBgr.size();
            const size_t stripCount = (std::min<size_t>)(32, totalDeck);
            m_filmstripBitmaps.reserve(stripCount);
            for (size_t si = 0; si < stripCount; ++si) {
                const size_t srcIdx = (stripCount <= 1)
                    ? 0
                    : ((si * (totalDeck - 1) + (stripCount - 1) / 2) / (stripCount - 1));
                const cv::Mat& srcBgr = m_previewDeckBgr[srcIdx];
                if (srcBgr.empty()) continue;
                constexpr int kStripH = 64;
                const double aspect = static_cast<double>(srcBgr.cols) / static_cast<double>((std::max)(1, srcBgr.rows));
                const int stripW = std::clamp(static_cast<int>(std::round(kStripH * aspect)), 48, 136);
                cv::Mat resizedBgr, bgra;
                cv::resize(srcBgr, resizedBgr, cv::Size(stripW, kStripH), 0.0, 0.0, cv::INTER_AREA);
                cv::cvtColor(resizedBgr, bgra, cv::COLOR_BGR2BGRA);
                ComPtr<ID2D1Bitmap> bmp;
                if (SUCCEEDED(m_renderTarget->CreateBitmap(
                        D2D1::SizeU(static_cast<UINT32>(bgra.cols), static_cast<UINT32>(bgra.rows)),
                        bgra.data,
                        static_cast<UINT32>(bgra.step[0]),
                        &bp,
                        bmp.GetAddressOf())) && bmp) {
                    m_filmstripBitmaps.push_back(std::move(bmp));
                }
            }
        }
        m_filmstripDirty = false;
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
    for (auto& bmp : m_recentLogoThumbBitmaps) bmp.Reset();
    m_filmstripBitmaps.clear();
    m_frameBitmap.Reset();
    m_bitmapW = 0;
    m_bitmapH = 0;
    m_renderTarget.Reset();
    m_composedDirty = true;
    m_wallpaperThumbsDirty = true;
    m_recentLogoThumbsDirty = true;
    m_filmstripDirty = true;
}

void StudioPreviewWindow::insertAnnotationCardPreset(int cardIndex, const float* dropNormX, const float* dropNormY) {
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
        const float cxNorm = dropNormX ? std::clamp(*dropNormX, 0.08f, 0.92f) : 0.5f;
        const float cyNorm = dropNormY ? std::clamp(*dropNormY, 0.08f, 0.92f) : 0.5f;
        z.focusX = static_cast<float>(m_timeline.sourceWidth) * cxNorm;
        z.focusY = static_cast<float>(m_timeline.sourceHeight) * cyNorm;
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
            ann.customStepText = false;
            ann.text = "1";
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

    if (dropNormX && dropNormY) {
        const float spanW = ann.x1 - ann.x0;
        const float spanH = ann.y1 - ann.y0;
        const float cx = std::clamp(*dropNormX, 0.04f, 0.96f);
        const float cy = std::clamp(*dropNormY, 0.04f, 0.96f);
        if (isPointDirectedAnnotation(ann.kind)) {
            // 指向型箭头/线段：让箭头尖端 (x1, y1) 精准对准用户松手目标点
            ann.x1 = cx;
            ann.y1 = cy;
            ann.x0 = std::clamp(cx - spanW, 0.02f, 0.98f);
            ann.y0 = std::clamp(cy - spanH, 0.02f, 0.98f);
        } else {
            const float halfW = std::abs(spanW) * 0.5f;
            const float halfH = std::abs(spanH) * 0.5f;
            ann.x0 = std::clamp(cx - halfW, 0.01f, 0.99f - halfW * 2.0f);
            ann.y0 = std::clamp(cy - halfH, 0.01f, 0.99f - halfH * 2.0f);
            ann.x1 = std::clamp(ann.x0 + std::abs(spanW), 0.02f, 0.99f);
            ann.y1 = std::clamp(ann.y0 + std::abs(spanH), 0.02f, 0.99f);
        }
    }

    m_timeline.annotations.push_back(ann);
    m_selectedAnnIdx = static_cast<int>(m_timeline.annotations.size()) - 1;
    m_selectedZoomIdx = -1;
    m_selectedClipIdx = -1;
    m_editingAnnotationText = (cardIndex >= 0 && cardIndex <= 4);
    resequenceAutoStepBadges();
    m_annDemoStartTick = (ann.kind == StudioAnnotationKind::BlurMask) ? 0 : GetTickCount64();

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
        if (!fmt || txt.empty() || r.right <= r.left || r.bottom <= r.top) return;
        fmt->SetTextAlignment(hAlign);
        fmt->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
        setBrush(col);
        m_renderTarget->DrawText(txt.c_str(), static_cast<UINT32>(txt.size()), fmt, r, brush, D2D1_DRAW_TEXT_OPTIONS_CLIP);
    };

    const float fullW = padR - padL;
    float y = startY;
    const float tilePhase = static_cast<float>(GetTickCount64() % 1400ULL) / 1400.0f;
    const float tileSin = std::sin(tilePhase * 6.2831853f);

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
            const bool hov = (m_hoveredHitId == hId);
            m_hitZones.push_back({hId, rc, i});

            setBrush(colTileBg);
            m_renderTarget->FillRoundedRectangle(D2D1::RoundedRect(rc, 6.5f, 6.5f), brush);
            setBrush(hov ? colAccent : colBorder);
            m_renderTarget->DrawRoundedRectangle(D2D1::RoundedRect(rc, 6.5f, 6.5f), brush, hov ? 1.5f : 0.9f);

            const float liftY = hov ? (-1.8f * std::abs(tileSin)) : 0.0f;
            const D2D1_RECT_F inner = D2D1::RectF(rc.left + 9.0f, rc.top + 6.5f + liftY, rc.right - 9.0f, rc.bottom - 6.5f + liftY);
            if (i == 0) {
                drawTxt(L"Text", m_bodyFmt.Get(), D2D1::RectF(rc.left, rc.top + liftY, rc.right, rc.bottom + liftY), colPrimary, DWRITE_TEXT_ALIGNMENT_CENTER);
            } else if (i == 1) {
                setBrush(D2D1::ColorF(0.18f, 0.22f, 0.30f, 1.0f));
                m_renderTarget->FillRoundedRectangle(D2D1::RoundedRect(inner, 6.0f, 6.0f), brush);
                drawTxt(L"Text", m_captionFmt.Get(), inner, D2D1::ColorF(1, 1, 1, 1), DWRITE_TEXT_ALIGNMENT_CENTER);
            } else if (i == 2) {
                const float bob = hov ? (tileSin * 1.8f) : 0.0f;
                const D2D1_RECT_F brc = D2D1::RectF(inner.left, inner.top - 1.0f + bob, inner.right, inner.bottom - 3.0f + bob);
                setBrush(D2D1::ColorF(0.66f, 0.33f, 0.97f, 1.0f));
                m_renderTarget->FillRoundedRectangle(D2D1::RoundedRect(brc, 7.0f, 7.0f), brush);
                const float bcx = (brc.left + brc.right) * 0.5f;
                m_renderTarget->DrawLine({bcx - 3.0f, brc.bottom}, {bcx, brc.bottom + 4.0f}, brush, 2.2f);
                m_renderTarget->DrawLine({bcx + 3.0f, brc.bottom}, {bcx, brc.bottom + 4.0f}, brush, 2.2f);
                drawTxt(L"Text", m_captionFmt.Get(), brc, D2D1::ColorF(1, 1, 1, 1), DWRITE_TEXT_ALIGNMENT_CENTER);
            } else if (i == 3) {
                D2D1_STROKE_STYLE_PROPERTIES sp = D2D1::StrokeStyleProperties();
                sp.dashStyle = D2D1_DASH_STYLE_DASH;
                sp.dashOffset = hov ? (tilePhase * 6.0f) : 0.0f;
                ComPtr<ID2D1StrokeStyle> ds;
                m_d2dFactory->CreateStrokeStyle(sp, nullptr, 0, ds.GetAddressOf());
                setBrush(hov ? colAccent : colSecondary);
                m_renderTarget->DrawRoundedRectangle(D2D1::RoundedRect(inner, 5.0f, 5.0f), brush, 1.3f, ds.Get());
                drawTxt(L"Text", m_captionFmt.Get(), inner, colPrimary, DWRITE_TEXT_ALIGNMENT_CENTER);
            } else if (i == 4) {
                const float glowAlpha = hov ? (0.28f + 0.34f * (0.5f + 0.5f * tileSin)) : 0.35f;
                setBrush(D2D1::ColorF(0.93f, 0.28f, 0.60f, glowAlpha));
                m_renderTarget->DrawRoundedRectangle(D2D1::RoundedRect(inner, 5.5f, 5.5f), brush, hov ? 4.4f : 3.4f);
                setBrush(D2D1::ColorF(0.93f, 0.28f, 0.60f, 1.0f));
                m_renderTarget->DrawRoundedRectangle(D2D1::RoundedRect(inner, 5.5f, 5.5f), brush, 1.4f);
                drawTxt(L"Text", m_captionFmt.Get(), inner, colPrimary, DWRITE_TEXT_ALIGNMENT_CENTER);
            } else {
                const float cx = (rc.left + rc.right) * 0.5f;
                const float cy = (rc.top + rc.bottom) * 0.5f;
                if (hov) {
                    setBrush(D2D1::ColorF(0.23f, 0.51f, 0.96f, (1.0f - tilePhase) * 0.60f));
                    m_renderTarget->DrawEllipse(D2D1::Ellipse({cx, cy}, 11.5f + tilePhase * 6.0f, 11.5f + tilePhase * 6.0f), brush, 1.5f);
                }
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
        ComPtr<ID2D1StrokeStyle> roundCapStyle;
        if (m_d2dFactory) {
            D2D1_STROKE_STYLE_PROPERTIES rsp = D2D1::StrokeStyleProperties(
                D2D1_CAP_STYLE_ROUND, D2D1_CAP_STYLE_ROUND, D2D1_CAP_STYLE_ROUND, D2D1_LINE_JOIN_ROUND);
            m_d2dFactory->CreateStrokeStyle(rsp, nullptr, 0, roundCapStyle.GetAddressOf());
        }
        for (int i = 0; i < 9; ++i) {
            const int cardIdx = 6 + i;
            const int col = i % 3;
            const int row = i / 3;
            const float tx = padL + col * (cw + gap);
            const float ty = y + row * (ch + gap);
            const D2D1_RECT_F rc = D2D1::RectF(tx, ty, tx + cw, ty + ch);
            const int hId = Hit_AnnCardBase + cardIdx;
            const bool hov = (m_hoveredHitId == hId);
            m_hitZones.push_back({hId, rc, cardIdx});

            setBrush(colTileBg);
            m_renderTarget->FillRoundedRectangle(D2D1::RoundedRect(rc, 6.5f, 6.5f), brush);
            setBrush(hov ? colAccent : colBorder);
            m_renderTarget->DrawRoundedRectangle(D2D1::RoundedRect(rc, 6.5f, 6.5f), brush, hov ? 1.5f : 0.9f);

            const float cx = (rc.left + rc.right) * 0.5f;
            const float cy = (rc.top + rc.bottom) * 0.5f;
            if (i == 0) {
                setBrush(D2D1::ColorF(0.06f, 0.73f, 0.51f, 0.30f));
                m_renderTarget->DrawLine({cx - 17.0f, cy}, {cx + 17.0f, cy}, brush, 6.0f, roundCapStyle.Get());
                setBrush(D2D1::ColorF(0.06f, 0.73f, 0.51f, 1.0f));
                m_renderTarget->DrawLine({cx - 17.0f, cy}, {cx + 17.0f, cy}, brush, 2.4f, roundCapStyle.Get());
                if (hov) {
                    const float sx = (cx - 16.0f) + tilePhase * 26.0f;
                    setBrush(D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.90f));
                    m_renderTarget->DrawLine({sx, cy}, {(std::min)(cx + 16.0f, sx + 6.5f), cy}, brush, 2.2f, roundCapStyle.Get());
                }
            } else if (i == 1) {
                D2D1_STROKE_STYLE_PROPERTIES sp = D2D1::StrokeStyleProperties(
                    D2D1_CAP_STYLE_ROUND, D2D1_CAP_STYLE_ROUND, D2D1_CAP_STYLE_ROUND);
                sp.dashStyle = D2D1_DASH_STYLE_DASH;
                sp.dashOffset = hov ? (-tilePhase * 8.0f) : 0.0f;
                ComPtr<ID2D1StrokeStyle> ds;
                m_d2dFactory->CreateStrokeStyle(sp, nullptr, 0, ds.GetAddressOf());
                setBrush(D2D1::ColorF(0.22f, 0.74f, 0.97f, 1.0f));
                m_renderTarget->DrawLine({cx - 17.0f, cy}, {cx + 17.0f, cy}, brush, 2.4f, ds.Get());
            } else if (i == 2) {
                const float dx = hov ? (tileSin * 2.8f) : 0.0f;
                setBrush(D2D1::ColorF(0.96f, 0.25f, 0.37f, 1.0f));
                m_renderTarget->DrawLine({cx - 17.0f + dx, cy}, {cx + 15.5f + dx, cy}, brush, 2.3f, roundCapStyle.Get());
                m_renderTarget->DrawLine({cx + 9.0f + dx, cy - 5.2f}, {cx + 16.5f + dx, cy}, brush, 2.3f, roundCapStyle.Get());
                m_renderTarget->DrawLine({cx + 9.0f + dx, cy + 5.2f}, {cx + 16.5f + dx, cy}, brush, 2.3f, roundCapStyle.Get());
            } else if (i == 3) {
                if (hov) {
                    const float exp = tilePhase * 4.5f;
                    setBrush(D2D1::ColorF(0.94f, 0.27f, 0.27f, (1.0f - tilePhase) * 0.55f));
                    m_renderTarget->DrawRectangle(D2D1::RectF(cx - 10.5f - exp, cy - 9.5f - exp, cx + 10.5f + exp, cy + 9.5f + exp), brush, 1.3f);
                }
                setBrush(D2D1::ColorF(0.94f, 0.27f, 0.27f, 1.0f));
                m_renderTarget->DrawRectangle(D2D1::RectF(cx - 10.5f, cy - 9.5f, cx + 10.5f, cy + 9.5f), brush, 2.3f);
            } else if (i == 4) {
                if (hov) {
                    const float exp = tilePhase * 4.5f;
                    setBrush(D2D1::ColorF(0.96f, 0.62f, 0.04f, (1.0f - tilePhase) * 0.55f));
                    m_renderTarget->DrawRoundedRectangle(
                        D2D1::RoundedRect(D2D1::RectF(cx - 10.5f - exp, cy - 9.5f - exp, cx + 10.5f + exp, cy + 9.5f + exp), 5.0f + exp, 5.0f + exp), brush, 1.3f);
                }
                setBrush(D2D1::ColorF(0.96f, 0.62f, 0.04f, 1.0f));
                m_renderTarget->DrawRoundedRectangle(
                    D2D1::RoundedRect(D2D1::RectF(cx - 10.5f, cy - 9.5f, cx + 10.5f, cy + 9.5f), 5.0f, 5.0f), brush, 2.3f);
            } else if (i == 5) {
                if (hov) {
                    const float exp = tilePhase * 5.0f;
                    setBrush(D2D1::ColorF(colPrimary.r, colPrimary.g, colPrimary.b, (1.0f - tilePhase) * 0.50f));
                    m_renderTarget->DrawEllipse(D2D1::Ellipse({cx, cy}, 10.2f + exp, 10.2f + exp), brush, 1.3f);
                }
                setBrush(colPrimary);
                m_renderTarget->DrawEllipse(D2D1::Ellipse({cx, cy}, 10.2f, 10.2f), brush, 2.4f);
            } else if (i == 6) {
                // 手绘重点圈图标：圆润单笔螺旋收尾马克笔弧线（与舞台真实渲染完全同构）
                ComPtr<ID2D1PathGeometry> loopGeo;
                if (m_d2dFactory && SUCCEEDED(m_d2dFactory->CreatePathGeometry(loopGeo.GetAddressOf())) && loopGeo) {
                    ComPtr<ID2D1GeometrySink> sk;
                    if (SUCCEEDED(loopGeo->Open(sk.GetAddressOf())) && sk) {
                        const int maxS = hov ? (std::max)(8, static_cast<int>(std::round(48.0f * std::min(1.0f, tilePhase * 1.45f)))) : 48;
                        auto sampleIconLoop = [&](int s) -> D2D1_POINT_2F {
                            const float u = static_cast<float>(s) / 48.0f;
                            const float th = -1.88f + u * 6.84f;
                            const float sp = 0.92f + 0.14f * u;
                            return {cx + std::cos(th) * 14.5f * sp, cy + std::sin(th) * 9.2f * sp};
                        };
                        sk->BeginFigure(sampleIconLoop(0), D2D1_FIGURE_BEGIN_HOLLOW);
                        for (int s = 1; s <= maxS; ++s) sk->AddLine(sampleIconLoop(s));
                        sk->EndFigure(D2D1_FIGURE_END_OPEN);
                        sk->Close();
                        setBrush(D2D1::ColorF(0.98f, 0.45f, 0.09f, 0.22f));
                        m_renderTarget->DrawGeometry(loopGeo.Get(), brush, 4.4f, roundCapStyle.Get());
                        setBrush(D2D1::ColorF(0.98f, 0.45f, 0.09f, 1.0f));
                        m_renderTarget->DrawGeometry(loopGeo.Get(), brush, 2.2f, roundCapStyle.Get());
                    }
                }
            } else if (i == 7) {
                const float dy = hov ? (tileSin * 2.6f) : 0.0f;
                setBrush(D2D1::ColorF(0.93f, 0.28f, 0.60f, 1.0f));
                ComPtr<ID2D1PathGeometry> geo;
                if (SUCCEEDED(m_d2dFactory->CreatePathGeometry(geo.GetAddressOf())) && geo) {
                    ComPtr<ID2D1GeometrySink> sk;
                    if (SUCCEEDED(geo->Open(sk.GetAddressOf())) && sk) {
                        sk->BeginFigure({cx - 3.8f, cy - 12.0f + dy}, D2D1_FIGURE_BEGIN_FILLED);
                        sk->AddLine({cx + 3.8f, cy - 12.0f + dy});
                        sk->AddLine({cx + 3.8f, cy + 1.0f + dy});
                        sk->AddLine({cx + 9.2f, cy + 1.0f + dy});
                        sk->AddLine({cx, cy + 12.0f + dy});
                        sk->AddLine({cx - 9.2f, cy + 1.0f + dy});
                        sk->AddLine({cx - 3.8f, cy + 1.0f + dy});
                        sk->EndFigure(D2D1_FIGURE_END_CLOSED);
                        sk->Close();
                        m_renderTarget->FillGeometry(geo.Get(), brush);
                    }
                }
            } else {
                // 点击手势图标：精致食指轻触矢量轮廓 + 指尖同心点击波纹弧
                const float tapY = hov ? (std::max)(0.0f, tileSin) * 1.8f : 0.0f;
                setBrush(D2D1::ColorF(0.93f, 0.28f, 0.60f, hov ? (1.0f - tilePhase) * 0.85f : 0.70f));
                const float ripR = hov ? (4.5f + tilePhase * 6.5f) : 5.5f;
                m_renderTarget->DrawEllipse(D2D1::Ellipse({cx - 1.5f, cy - 8.5f}, ripR, ripR * 0.72f), brush, 1.4f);

                ComPtr<ID2D1PathGeometry> handGeo;
                if (m_d2dFactory && SUCCEEDED(m_d2dFactory->CreatePathGeometry(handGeo.GetAddressOf())) && handGeo) {
                    ComPtr<ID2D1GeometrySink> sk;
                    if (SUCCEEDED(handGeo->Open(sk.GetAddressOf())) && sk) {
                        const float hy = cy + tapY;
                        sk->BeginFigure({cx - 3.5f, hy - 8.0f}, D2D1_FIGURE_BEGIN_FILLED);
                        sk->AddLine({cx + 0.5f, hy - 8.0f});
                        sk->AddLine({cx + 0.5f, hy - 1.0f});
                        sk->AddLine({cx + 7.5f, hy + 0.5f});
                        sk->AddLine({cx + 8.0f, hy + 8.5f});
                        sk->AddLine({cx + 4.0f, hy + 12.5f});
                        sk->AddLine({cx - 3.5f, hy + 12.5f});
                        sk->AddLine({cx - 8.0f, hy + 5.5f});
                        sk->AddLine({cx - 5.5f, hy + 3.0f});
                        sk->AddLine({cx - 3.5f, hy + 5.0f});
                        sk->EndFigure(D2D1_FIGURE_END_CLOSED);
                        sk->Close();
                        setBrush(colPrimary);
                        m_renderTarget->DrawGeometry(handGeo.Get(), brush, 1.8f, roundCapStyle.Get());
                    }
                }
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
        drawTxt(isZh ? L"点击或拖拽图标到右侧画面即可添加" : L"Click or drag icon onto video stage",
                m_captionFmt.Get(), D2D1::RectF(padL, y, padR, y + 22.0f), colSecondary);
    }

    // 悬停在任意标注卡片上时，在该卡片上方弹出清晰精致的名称气泡标签
    if (!m_draggingPresetToStage && m_hoveredHitId >= Hit_AnnCardBase && m_hoveredHitId < Hit_AnnCardBase + 19) {
        const int cardIdx = m_hoveredHitId - Hit_AnnCardBase;
        D2D1_RECT_F targetRc{};
        bool found = false;
        for (const auto& hz : m_hitZones) {
            if (hz.id == m_hoveredHitId) {
                targetRc = hz.rect;
                found = true;
                break;
            }
        }
        if (found) {
            const std::wstring tipLabel = annotationCardDisplayName(cardIdx, isZh);
            float estW = 0.0f;
            for (wchar_t wc : tipLabel) estW += (wc > 0x7F) ? 12.6f : 7.2f;
            const float pillW = std::clamp(estW + 20.0f, 54.0f, fullW);
            const float pillH = 25.0f;
            const float acx = (targetRc.left + targetRc.right) * 0.5f;
            const float pillL = std::clamp(acx - pillW * 0.5f, padL - 4.0f, padR + 4.0f - pillW);
            const bool placeBelow = (targetRc.top - pillH - 6.0f < startY - 4.0f);
            const float pillT = placeBelow ? (targetRc.bottom + 6.0f) : (targetRc.top - pillH - 6.0f);
            const D2D1_RECT_F pillRc = D2D1::RectF(pillL, pillT, pillL + pillW, pillT + pillH);

            // 柔和悬浮投影 + 高对比微晶底衬
            setBrush(D2D1::ColorF(0.02f, 0.04f, 0.09f, dark ? 0.45f : 0.18f));
            m_renderTarget->FillRoundedRectangle(
                D2D1::RoundedRect(D2D1::RectF(pillRc.left - 1.0f, pillRc.top + 2.0f, pillRc.right + 1.0f, pillRc.bottom + 3.0f), 7.0f, 7.0f),
                brush);

            const D2D1_COLOR_F tipBg = dark
                ? D2D1::ColorF(0.16f, 0.20f, 0.29f, 0.98f)
                : D2D1::ColorF(0.09f, 0.13f, 0.21f, 0.95f);
            setBrush(tipBg);
            m_renderTarget->FillRoundedRectangle(D2D1::RoundedRect(pillRc, 6.0f, 6.0f), brush);

            const float tipX = std::clamp(acx, pillRc.left + 10.0f, pillRc.right - 10.0f);
            ComPtr<ID2D1PathGeometry> caret;
            if (m_d2dFactory && SUCCEEDED(m_d2dFactory->CreatePathGeometry(caret.GetAddressOf())) && caret) {
                ComPtr<ID2D1GeometrySink> sk;
                if (SUCCEEDED(caret->Open(sk.GetAddressOf())) && sk) {
                    if (placeBelow) {
                        sk->BeginFigure({tipX - 4.5f, pillRc.top + 0.5f}, D2D1_FIGURE_BEGIN_FILLED);
                        sk->AddLine({tipX, pillRc.top - 4.5f});
                        sk->AddLine({tipX + 4.5f, pillRc.top + 0.5f});
                    } else {
                        sk->BeginFigure({tipX - 4.5f, pillRc.bottom - 0.5f}, D2D1_FIGURE_BEGIN_FILLED);
                        sk->AddLine({tipX, pillRc.bottom + 4.5f});
                        sk->AddLine({tipX + 4.5f, pillRc.bottom - 0.5f});
                    }
                    sk->EndFigure(D2D1_FIGURE_END_CLOSED);
                    sk->Close();
                    m_renderTarget->FillGeometry(caret.Get(), brush);
                }
            }
            setBrush(D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.16f));
            m_renderTarget->DrawRoundedRectangle(D2D1::RoundedRect(pillRc, 6.0f, 6.0f), brush, 0.9f);
            drawTxt(tipLabel, m_captionFmt.Get(), pillRc, D2D1::ColorF(0.98f, 0.99f, 1.0f, 1.0f), DWRITE_TEXT_ALIGNMENT_CENTER);
        }
    }
}

void StudioPreviewWindow::renderWatermarkDrawer(
    float padL, float padR, float startY, float bottomLimit,
    bool dark, bool isZh, ID2D1SolidColorBrush* brush) {
    const auto accentRgb = tools3000::core::NativeThemePalette::getEffectiveAccentColorRGB();
    const D2D1_COLOR_F colAccent = D2D1::ColorF(accentRgb.r, accentRgb.g, accentRgb.b, 1.0f);
    const D2D1_COLOR_F colBorder = dark ? D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.11f) : D2D1::ColorF(0.059f, 0.090f, 0.165f, 0.10f);
    const D2D1_COLOR_F colPrimary = dark ? D2D1::ColorF(0.96f, 0.97f, 0.99f, 1.0f) : D2D1::ColorF(0.059f, 0.090f, 0.165f, 1.0f);
    const D2D1_COLOR_F colSecondary = dark ? D2D1::ColorF(0.62f, 0.68f, 0.78f, 1.0f) : D2D1::ColorF(0.32f, 0.38f, 0.47f, 1.0f);
    const D2D1_COLOR_F colFieldBg = dark ? D2D1::ColorF(0.065f, 0.082f, 0.122f, 0.96f) : D2D1::ColorF(0.948f, 0.960f, 0.980f, 1.0f);

    auto setBrush = [&](const D2D1_COLOR_F& c) { brush->SetColor(c); };
    auto drawTxt = [&](const std::wstring& txt, IDWriteTextFormat* fmt, const D2D1_RECT_F& r,
                       const D2D1_COLOR_F& col,
                       DWRITE_TEXT_ALIGNMENT hAlign = DWRITE_TEXT_ALIGNMENT_LEADING) {
        if (!fmt || txt.empty() || r.right <= r.left || r.bottom <= r.top) return;
        fmt->SetTextAlignment(hAlign);
        fmt->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
        setBrush(col);
        m_renderTarget->DrawText(txt.c_str(), static_cast<UINT32>(txt.size()), fmt, r, brush, D2D1_DRAW_TEXT_OPTIONS_CLIP);
    };

    const float fullW = padR - padL;
    float y = startY;
    auto& wm = m_config.watermark;

    // 1. 顶部开关行（支持企业组策略只读锁定徽章）
    drawTxt(isZh ? L"启用画面水印" : L"Enable Watermark", m_sectionFmt.Get(),
            D2D1::RectF(padL, y, padR - 46.0f, y + 20.0f), colPrimary);
    {
        const D2D1_RECT_F swRc = D2D1::RectF(padR - 36.0f, y + 0.5f, padR, y + 19.5f);
        if (!wm.policyForced) {
            m_hitZones.push_back({Hit_WatermarkToggle, swRc, -1});
        }
        setBrush(wm.enabled ? colAccent : (dark ? D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.16f) : D2D1::ColorF(0.75f, 0.80f, 0.86f, 1.0f)));
        m_renderTarget->FillRoundedRectangle(D2D1::RoundedRect(swRc, 9.5f, 9.5f), brush);
        const float kx = wm.enabled ? (swRc.right - 9.5f) : (swRc.left + 9.5f);
        setBrush(D2D1::ColorF(0.0f, 0.0f, 0.0f, 0.18f));
        m_renderTarget->FillEllipse(D2D1::Ellipse({kx, y + 10.8f}, 7.0f, 7.0f), brush);
        setBrush(D2D1::ColorF(1.0f, 1.0f, 1.0f, wm.policyForced ? 0.78f : 1.0f));
        m_renderTarget->FillEllipse(D2D1::Ellipse({kx, y + 10.0f}, 6.8f, 6.8f), brush);
    }
    y += 25.0f;

    if (wm.policyForced) {
        drawTxt(isZh ? L"[企业合规策略已强制锁定开启]" : L"[Enforced by Enterprise Policy]",
                m_captionFmt.Get(), D2D1::RectF(padL, y - 3.0f, padR, y + 15.0f), colAccent);
        y += 18.0f;
    }

    // 2. 一体式凹槽分段器：图片 Logo | 文字角标 | 企业平铺
    {
        const D2D1_RECT_F trough = D2D1::RectF(padL, y, padR, y + 28.0f);
        setBrush(dark ? D2D1::ColorF(0.045f, 0.058f, 0.090f, 0.95f) : D2D1::ColorF(0.918f, 0.935f, 0.962f, 1.0f));
        m_renderTarget->FillRoundedRectangle(D2D1::RoundedRect(trough, 7.5f, 7.5f), brush);
        setBrush(colBorder);
        m_renderTarget->DrawRoundedRectangle(D2D1::RoundedRect(trough, 7.5f, 7.5f), brush, 0.8f);

        static const wchar_t* modeZh[3] = { L"图片 Logo", L"文字角标", L"企业平铺" };
        static const wchar_t* modeEn[3] = { L"Logo Img", L"Corner Txt", L"Tiled DLP" };
        const float cellW = (fullW - 6.0f) / 3.0f;
        for (int m = 0; m < 3; ++m) {
            const D2D1_RECT_F crc = D2D1::RectF(
                trough.left + 3.0f + m * cellW, trough.top + 2.5f,
                trough.left + 3.0f + (m + 1) * cellW, trough.bottom - 2.5f);
            m_hitZones.push_back({Hit_WatermarkModeBase + m, crc, m});
            const bool act = (static_cast<int>(wm.mode) == m);
            if (act) {
                setBrush(D2D1::ColorF(0.02f, 0.04f, 0.10f, dark ? 0.36f : 0.09f));
                m_renderTarget->FillRoundedRectangle(
                    D2D1::RoundedRect(D2D1::RectF(crc.left, crc.top + 1.0f, crc.right, crc.bottom + 1.2f), 5.5f, 5.5f), brush);
                fillLinearVGradRounded(m_renderTarget.Get(), crc, 5.5f,
                    dark ? D2D1::ColorF(0.165f, 0.205f, 0.295f, 1.0f) : D2D1::ColorF(1.0f, 1.0f, 1.0f, 1.0f),
                    dark ? D2D1::ColorF(0.130f, 0.165f, 0.245f, 1.0f) : D2D1::ColorF(0.975f, 0.985f, 1.0f, 1.0f));
                setBrush(dark ? D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.14f) : D2D1::ColorF(0.06f, 0.10f, 0.20f, 0.09f));
                m_renderTarget->DrawRoundedRectangle(D2D1::RoundedRect(crc, 5.5f, 5.5f), brush, 0.9f);
            }
            drawTxt(isZh ? modeZh[m] : modeEn[m], m_captionFmt.Get(), crc,
                    act ? colPrimary : colSecondary, DWRITE_TEXT_ALIGNMENT_CENTER);
        }
    }
    y += 35.0f;

    if (wm.mode == WatermarkMode::ImageLogo) {
        // 导入图片按钮 + 当前 Logo 文件名展示
        const D2D1_RECT_F pickBtn = D2D1::RectF(padL, y, padR, y + 32.0f);
        m_hitZones.push_back({Hit_WatermarkPickImage, pickBtn, -1});
        const bool hovPick = (m_hoveredHitId == Hit_WatermarkPickImage);
        fillLinearVGradRounded(m_renderTarget.Get(), pickBtn, 7.0f,
            dark ? D2D1::ColorF(0.14f, 0.18f, 0.27f, 1.0f) : D2D1::ColorF(1.0f, 1.0f, 1.0f, 1.0f),
            dark ? D2D1::ColorF(0.11f, 0.14f, 0.22f, 1.0f) : D2D1::ColorF(0.965f, 0.978f, 0.996f, 1.0f));
        setBrush(hovPick ? colAccent : colBorder);
        m_renderTarget->DrawRoundedRectangle(D2D1::RoundedRect(pickBtn, 7.0f, 7.0f), brush, hovPick ? 1.4f : 1.0f);

        std::wstring btnText = isZh ? L"+ 导入透明图片 / 企业 Logo" : L"+ Import Transparent Logo";
        if (!wm.imagePathUtf8.empty()) {
            const std::wstring fname = std::filesystem::path(tools3000::core::WinUtils::utf8ToWstring(wm.imagePathUtf8)).filename().wstring();
            if (!fname.empty()) {
                btnText = (isZh ? L"更换图片: " : L"Change: ") + fname;
            }
        }
        drawTxt(btnText, m_captionFmt.Get(), D2D1::RectF(pickBtn.left + 10.0f, pickBtn.top, pickBtn.right - 10.0f, pickBtn.bottom),
                hovPick ? colAccent : colPrimary, DWRITE_TEXT_ALIGNMENT_CENTER);
        y += 39.0f;

        // 历史常用 Logo 快捷缩略图宫格 (最多 6 张) 或空状态引导提示
        if (!m_recentLogoPaths.empty()) {
            drawTxt(isZh ? L"最近使用的图片徽标" : L"Recent Logos", m_captionFmt.Get(),
                    D2D1::RectF(padL, y, padR, y + 17.0f), colSecondary);
            y += 20.0f;
            const float gap = 6.0f;
            const float cellSz = (fullW - gap * 5.0f) / 6.0f;
            for (size_t i = 0; i < m_recentLogoPaths.size() && i < 6; ++i) {
                const float lx = padL + static_cast<float>(i) * (cellSz + gap);
                const D2D1_RECT_F trc = D2D1::RectF(lx, y, lx + cellSz, y + cellSz);
                const int hId = Hit_WatermarkRecentBase + static_cast<int>(i);
                m_hitZones.push_back({hId, trc, static_cast<int>(i)});

                setBrush(colFieldBg);
                m_renderTarget->FillRoundedRectangle(D2D1::RoundedRect(trc, 6.0f, 6.0f), brush);
                if (m_recentLogoThumbBitmaps[i]) {
                    const D2D1_RECT_F imgRc = D2D1::RectF(trc.left + 3.0f, trc.top + 3.0f, trc.right - 3.0f, trc.bottom - 3.0f);
                    m_renderTarget->DrawBitmap(m_recentLogoThumbBitmaps[i].Get(), imgRc, 1.0f, D2D1_BITMAP_INTERPOLATION_MODE_LINEAR);
                }
                const bool sel = (wm.imagePathUtf8 == m_recentLogoPaths[i]);
                setBrush(sel ? colAccent : (m_hoveredHitId == hId ? colAccent : colBorder));
                m_renderTarget->DrawRoundedRectangle(D2D1::RoundedRect(trc, 6.0f, 6.0f), brush, sel ? 1.8f : 1.0f);
            }
            y += cellSz + 10.0f;
        } else if (wm.imagePathUtf8.empty()) {
            drawTxt(isZh ? L"[提示] 请先点击上方按钮选择 PNG / WebP 水印图片" : L"[Info] Import a PNG / WebP image above first",
                    m_captionFmt.Get(), D2D1::RectF(padL, y, padR, y + 19.0f), colAccent);
            y += 24.0f;
        }
    } else {
        // 文字模板编辑框 + 动态占位符智能双向切换胶囊
        drawTxt(isZh ? L"水印文字内容 (支持动态变量)" : L"Watermark Text & Tokens", m_captionFmt.Get(),
                D2D1::RectF(padL, y, padR, y + 17.0f), colSecondary);
        y += 20.0f;

        const D2D1_RECT_F txtBox = D2D1::RectF(padL, y, padR, y + 30.0f);
        m_hitZones.push_back({Hit_WatermarkTextInput, txtBox, -1});
        setBrush(colFieldBg);
        m_renderTarget->FillRoundedRectangle(D2D1::RoundedRect(txtBox, 6.5f, 6.5f), brush);
        setBrush(m_editingWatermarkText ? colAccent : colBorder);
        m_renderTarget->DrawRoundedRectangle(D2D1::RoundedRect(txtBox, 6.5f, 6.5f), brush, m_editingWatermarkText ? 1.5f : 1.0f);

        std::wstring shown = tools3000::core::WinUtils::utf8ToWstring(wm.textTemplate);
        if (m_editingWatermarkText && ((GetTickCount() / 500) % 2 == 0)) {
            shown.push_back(L'|');
        }
        drawTxt(shown.empty() ? (isZh ? L"点击输入文字或点下方变量..." : L"Type text or toggle token...") : shown,
                m_bodyFmt.Get(), D2D1::RectF(txtBox.left + 8.0f, txtBox.top, txtBox.right - 8.0f, txtBox.bottom), colPrimary);
        y += 35.0f;

        // 4 枚智能变量胶囊（已包含时高亮激活，点击一键添加或移除）
        static const wchar_t* tokLblZh[4] = { L"+用户名", L"+设备名", L"+日期时间", L"+严禁外传" };
        static const wchar_t* tokActZh[4] = { L"用户名", L"设备名", L"日期时间", L"严禁外传" };
        static const wchar_t* tokLblEn[4] = { L"+User", L"+Host", L"+Date", L"+Confid." };
        static const wchar_t* tokActEn[4] = { L"User", L"Host", L"Date", L"Confid." };
        auto hasTokenInTemplate = [&](int idx) -> bool {
            const std::string& s = wm.textTemplate;
            if (idx == 0) return s.find("{user}") != std::string::npos || s.find("{USER}") != std::string::npos;
            if (idx == 1) return s.find("{host}") != std::string::npos || s.find("{HOST}") != std::string::npos;
            if (idx == 2) return s.find("{date}") != std::string::npos || s.find("{time}") != std::string::npos || s.find("{datetime}") != std::string::npos;
            return s.find("严禁外传") != std::string::npos || s.find("CONFIDENTIAL") != std::string::npos;
        };
        const float tGap = 4.5f;
        const float tw = (fullW - tGap * 3.0f) / 4.0f;
        for (int k = 0; k < 4; ++k) {
            const D2D1_RECT_F krc = D2D1::RectF(padL + k * (tw + tGap), y, padL + k * (tw + tGap) + tw, y + 24.0f);
            const int hId = Hit_WatermarkTokenBase + k;
            m_hitZones.push_back({hId, krc, k});
            const bool tokOn = hasTokenInTemplate(k);
            const bool hov = (m_hoveredHitId == hId);
            setBrush(tokOn
                ? D2D1::ColorF(accentRgb.r, accentRgb.g, accentRgb.b, dark ? 0.24f : 0.13f)
                : (hov ? D2D1::ColorF(accentRgb.r, accentRgb.g, accentRgb.b, 0.10f) : colFieldBg));
            m_renderTarget->FillRoundedRectangle(D2D1::RoundedRect(krc, 5.5f, 5.5f), brush);
            setBrush((tokOn || hov) ? colAccent : colBorder);
            m_renderTarget->DrawRoundedRectangle(D2D1::RoundedRect(krc, 5.5f, 5.5f), brush, tokOn ? 1.2f : 0.9f);
            const wchar_t* label = tokOn ? (isZh ? tokActZh[k] : tokActEn[k])
                                         : (isZh ? tokLblZh[k] : tokLblEn[k]);
            drawTxt(label, m_captionFmt.Get(), krc,
                    (tokOn || hov) ? colAccent : colSecondary, DWRITE_TEXT_ALIGNMENT_CENTER);
        }
        y += 32.0f;
    }

    // 3. 锚定位置一体式分段器（仅在单点图片或文字角标模式下展示，同时支持右侧画面直接拖拽自由定位）
    if (wm.mode != WatermarkMode::TiledTraceability) {
        drawTxt(isZh ? L"对齐位置 (亦可直接在右侧画面拖拽)" : L"Position (Or drag directly on stage)",
                m_captionFmt.Get(), D2D1::RectF(padL, y, padR, y + 17.0f), colSecondary);
        y += 20.0f;
        const D2D1_RECT_F trough = D2D1::RectF(padL, y, padR, y + 26.0f);
        setBrush(dark ? D2D1::ColorF(0.045f, 0.058f, 0.090f, 0.95f) : D2D1::ColorF(0.918f, 0.935f, 0.962f, 1.0f));
        m_renderTarget->FillRoundedRectangle(D2D1::RoundedRect(trough, 6.5f, 6.5f), brush);

        static const wchar_t* posZh[5] = { L"左上", L"右上", L"左下", L"右下", L"居中" };
        static const wchar_t* posEn[5] = { L"TL", L"TR", L"BL", L"BR", L"Mid" };
        const float pw = (fullW - 5.0f) / 5.0f;
        for (int p = 0; p < 5; ++p) {
            const D2D1_RECT_F prc = D2D1::RectF(trough.left + 2.5f + p * pw, trough.top + 2.2f,
                                                trough.left + 2.5f + (p + 1) * pw, trough.bottom - 2.2f);
            m_hitZones.push_back({Hit_WatermarkAnchorBase + p, prc, p});
            const bool act = (static_cast<int>(wm.anchor) == p);
            if (act) {
                fillLinearVGradRounded(m_renderTarget.Get(), prc, 4.8f,
                    dark ? D2D1::ColorF(0.165f, 0.205f, 0.295f, 1.0f) : D2D1::ColorF(1.0f, 1.0f, 1.0f, 1.0f),
                    dark ? D2D1::ColorF(0.130f, 0.165f, 0.245f, 1.0f) : D2D1::ColorF(0.975f, 0.985f, 1.0f, 1.0f));
                setBrush(colBorder);
                m_renderTarget->DrawRoundedRectangle(D2D1::RoundedRect(prc, 4.8f, 4.8f), brush, 0.85f);
            }
            drawTxt(isZh ? posZh[p] : posEn[p], m_captionFmt.Get(), prc,
                    act ? colPrimary : colSecondary, DWRITE_TEXT_ALIGNMENT_CENTER);
        }
        y += 33.0f;
    }

    // 4. 大小比例与不透明度滑块
    auto drawWmSlider = [&](SliderDragKind kind, int hitId, const std::wstring& lbl, float norm01, const std::wstring& vStr) {
        drawTxt(lbl, m_captionFmt.Get(), D2D1::RectF(padL, y, padL + 62.0f, y + 20.0f), colSecondary);
        const D2D1_RECT_F trk = D2D1::RectF(padL + 66.0f, y + 8.0f, padR - 48.0f, y + 12.0f);
        m_sliderTrackRects[static_cast<size_t>(kind)] = trk;
        m_hitZones.push_back({hitId, D2D1::RectF(trk.left - 5.0f, y, trk.right + 5.0f, y + 20.0f), -1});

        setBrush(dark ? D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.10f) : D2D1::ColorF(0.06f, 0.10f, 0.18f, 0.09f));
        m_renderTarget->FillRoundedRectangle(D2D1::RoundedRect(trk, 2.0f, 2.0f), brush);
        const float cl = std::clamp(norm01, 0.0f, 1.0f);
        const float kx = trk.left + (trk.right - trk.left) * cl;
        setBrush(colAccent);
        m_renderTarget->FillRoundedRectangle(
            D2D1::RoundedRect(D2D1::RectF(trk.left, trk.top, (std::max)(trk.left + 2.0f, kx), trk.bottom), 2.0f, 2.0f), brush);

        // 白瓷立体圆珠旋钮
        const float ky = (trk.top + trk.bottom) * 0.5f;
        setBrush(D2D1::ColorF(0.02f, 0.04f, 0.10f, 0.22f));
        m_renderTarget->FillEllipse(D2D1::Ellipse({kx, ky + 1.0f}, 6.2f, 6.2f), brush);
        setBrush(D2D1::ColorF(1.0f, 1.0f, 1.0f, 1.0f));
        m_renderTarget->FillEllipse(D2D1::Ellipse({kx, ky}, 5.8f, 5.8f), brush);
        setBrush(colAccent);
        m_renderTarget->DrawEllipse(D2D1::Ellipse({kx, ky}, 5.8f, 5.8f), brush, 1.4f);

        drawTxt(vStr, m_monoFmt.Get(), D2D1::RectF(padR - 44.0f, y, padR, y + 20.0f), colPrimary, DWRITE_TEXT_ALIGNMENT_TRAILING);
        y += 26.0f;
    };

    wchar_t sVal[24]{};
    std::swprintf(sVal, std::size(sVal), L"%d%%", static_cast<int>(std::round(wm.scale * 100.0f)));
    drawWmSlider(SliderDragKind::WatermarkScale, Hit_WatermarkSliderScale,
                 isZh ? L"水印大小" : L"Scale", (wm.scale - 0.35f) / (2.80f - 0.35f), sVal);

    std::swprintf(sVal, std::size(sVal), L"%d%%", static_cast<int>(std::round(wm.opacity * 100.0f)));
    drawWmSlider(SliderDragKind::WatermarkOpacity, Hit_WatermarkSliderOpacity,
                 isZh ? L"不透明度" : L"Opacity", (wm.opacity - 0.04f) / (1.0f - 0.04f), sVal);

    if (y + 36.0f <= bottomLimit) {
        y += 4.0f;
        const std::wstring hint = (wm.mode == WatermarkMode::TiledTraceability)
            ? (isZh ? L"平铺水印可自动适应深色与浅色画面背景，始终保持清晰柔和，且固定在顶层不随镜头缩放晃动。"
                    : L"Tiled watermark automatically adapts to both light and dark backgrounds, staying clear, subtle, and fixed above camera zoom.")
            : (isZh ? L"可在右侧预览画面直接按住鼠标拖拽水印至任意位置，或拖拽水印右下角控制点无级缩放；导出 MP4 与 GIF 均完整保留。"
                    : L"Drag the watermark directly on the preview stage to reposition, or drag its bottom-right handle to resize.");
        drawTxt(hint, m_captionWrapFmt.Get(), D2D1::RectF(padL, y, padR, bottomLimit), colSecondary);
    }
}

void StudioPreviewWindow::renderLeftRailAndDrawer(
    const D2D1_RECT_F& railRc, const D2D1_RECT_F& drawerRc,
    bool dark, bool isZh, ID2D1SolidColorBrush* brush) {
    const auto accentRgb = tools3000::core::NativeThemePalette::getEffectiveAccentColorRGB();
    const D2D1_COLOR_F colAccent = D2D1::ColorF(accentRgb.r, accentRgb.g, accentRgb.b, 1.0f);
    const D2D1_COLOR_F colAccentSoft = D2D1::ColorF(accentRgb.r, accentRgb.g, accentRgb.b, dark ? 0.20f : 0.11f);
    const D2D1_COLOR_F colCardBg = dark ? D2D1::ColorF(0.092f, 0.114f, 0.165f, 1.0f) : D2D1::ColorF(1.0f, 1.0f, 1.0f, 1.0f);
    const D2D1_COLOR_F colRailBg = dark ? D2D1::ColorF(0.068f, 0.084f, 0.125f, 1.0f) : D2D1::ColorF(0.942f, 0.955f, 0.978f, 1.0f);
    const D2D1_COLOR_F colBorder = dark ? D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.09f) : D2D1::ColorF(0.059f, 0.090f, 0.165f, 0.095f);
    const D2D1_COLOR_F colPrimary = dark ? D2D1::ColorF(0.96f, 0.97f, 0.99f, 1.0f) : D2D1::ColorF(0.059f, 0.090f, 0.165f, 1.0f);
    const D2D1_COLOR_F colSecondary = dark ? D2D1::ColorF(0.62f, 0.68f, 0.78f, 1.0f) : D2D1::ColorF(0.32f, 0.38f, 0.47f, 1.0f);
    const D2D1_COLOR_F colChipBg = dark ? D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.055f) : D2D1::ColorF(0.059f, 0.090f, 0.165f, 0.05f);

    const ComPtr<ID2D1StrokeStyle> roundSt = createRoundStrokeStyle(m_d2dFactory.Get());

    auto setBrush = [&](const D2D1_COLOR_F& c) { brush->SetColor(c); };
    auto drawTxt = [&](const std::wstring& txt, IDWriteTextFormat* fmt, const D2D1_RECT_F& r,
                       const D2D1_COLOR_F& col,
                       DWRITE_TEXT_ALIGNMENT hAlign = DWRITE_TEXT_ALIGNMENT_LEADING) {
        if (!fmt || txt.empty() || r.right <= r.left || r.bottom <= r.top) return;
        fmt->SetTextAlignment(hAlign);
        fmt->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
        setBrush(col);
        m_renderTarget->DrawText(txt.c_str(), static_cast<UINT32>(txt.size()), fmt, r, brush, D2D1_DRAW_TEXT_OPTIONS_CLIP);
    };

    setBrush(colRailBg);
    m_renderTarget->FillRoundedRectangle(D2D1::RoundedRect(railRc, 12.0f, 12.0f), brush);
    setBrush(colBorder);
    m_renderTarget->DrawRoundedRectangle(D2D1::RoundedRect(railRc, 12.0f, 12.0f), brush, 1.0f);

    static const wchar_t* tabShortZh[5] = { L"背景", L"标注", L"运镜", L"水印", L"字幕" };
    static const wchar_t* tabShortEn[5] = { L"Canvas", L"Mark", L"Cam", L"Brand", L"Sub" };
    for (int t = 0; t < 5; ++t) {
        const float by = railRc.top + 8.0f + t * 53.0f;
        const D2D1_RECT_F brc = D2D1::RectF(railRc.left + 4.0f, by, railRc.right - 4.0f, by + 46.0f);
        const int hId = Hit_LeftRailTabBase + t;
        m_hitZones.push_back({hId, brc, t});

        const bool active = (static_cast<int>(m_activeTab) == t);
        if (active) {
            setBrush(D2D1::ColorF(0.02f, 0.04f, 0.10f, dark ? 0.34f : 0.08f));
            m_renderTarget->FillRoundedRectangle(
                D2D1::RoundedRect(D2D1::RectF(brc.left, brc.top + 1.2f, brc.right, brc.bottom + 1.5f), 8.5f, 8.5f), brush);
            fillLinearVGradRounded(m_renderTarget.Get(), brc, 8.5f,
                dark ? D2D1::ColorF(0.155f, 0.195f, 0.285f, 1.0f) : D2D1::ColorF(1.0f, 1.0f, 1.0f, 1.0f),
                dark ? D2D1::ColorF(0.120f, 0.155f, 0.235f, 1.0f) : D2D1::ColorF(0.975f, 0.985f, 1.0f, 1.0f));
            setBrush(dark ? D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.13f) : D2D1::ColorF(0.06f, 0.10f, 0.20f, 0.09f));
            m_renderTarget->DrawRoundedRectangle(D2D1::RoundedRect(brc, 8.5f, 8.5f), brush, 0.9f);
            // 左侧 Linear 级垂直发光指示胶囊条
            setBrush(colAccent);
            m_renderTarget->FillRoundedRectangle(
                D2D1::RoundedRect(D2D1::RectF(brc.left + 0.5f, brc.top + 11.0f, brc.left + 3.1f, brc.bottom - 11.0f), 1.3f, 1.3f), brush);
        } else if (m_hoveredHitId == hId) {
            setBrush(colChipBg);
            m_renderTarget->FillRoundedRectangle(D2D1::RoundedRect(brc, 8.5f, 8.5f), brush);
        }

        const float icx = (brc.left + brc.right) * 0.5f;
        const float icy = brc.top + 14.5f;
        setBrush(active ? colAccent : colSecondary);
        if (t == 0) {
            m_renderTarget->DrawRoundedRectangle(
                D2D1::RoundedRect(D2D1::RectF(icx - 7.8f, icy - 6.2f, icx + 7.8f, icy + 6.2f), 2.5f, 2.5f), brush, 1.55f, roundSt.Get());
            m_renderTarget->FillEllipse(D2D1::Ellipse({icx - 3.0f, icy - 2.0f}, 1.5f, 1.5f), brush);
            m_renderTarget->DrawLine({icx - 5.8f, icy + 4.8f}, {icx - 0.8f, icy + 0.4f}, brush, 1.45f, roundSt.Get());
            m_renderTarget->DrawLine({icx - 0.8f, icy + 0.4f}, {icx + 5.8f, icy + 4.8f}, brush, 1.45f, roundSt.Get());
        } else if (t == 1) {
            drawCrispPenIcon(m_d2dFactory.Get(), m_renderTarget.Get(), brush, icx, icy, roundSt.Get());
        } else if (t == 2) {
            drawCrispCameraFocusIcon(m_renderTarget.Get(), brush, icx, icy, roundSt.Get());
        } else if (t == 3) {
            // 精密企业盾徽/印章矢量图标 (Watermark / Brand Shield)
            ComPtr<ID2D1PathGeometry> shGeo;
            if (m_d2dFactory && SUCCEEDED(m_d2dFactory->CreatePathGeometry(&shGeo)) && shGeo) {
                ComPtr<ID2D1GeometrySink> sk;
                if (SUCCEEDED(shGeo->Open(&sk)) && sk) {
                    sk->BeginFigure({icx, icy - 6.8f}, D2D1_FIGURE_BEGIN_HOLLOW);
                    sk->AddLine({icx + 6.5f, icy - 4.2f});
                    sk->AddLine({icx + 5.8f, icy + 1.8f});
                    sk->AddLine({icx, icy + 6.8f});
                    sk->AddLine({icx - 5.8f, icy + 1.8f});
                    sk->AddLine({icx - 6.5f, icy - 4.2f});
                    sk->EndFigure(D2D1_FIGURE_END_CLOSED);
                    sk->Close();
                    m_renderTarget->DrawGeometry(shGeo.Get(), brush, 1.55f, roundSt.Get());
                    m_renderTarget->DrawLine({icx - 2.4f, icy + 0.2f}, {icx - 0.4f, icy + 2.2f}, brush, 1.45f, roundSt.Get());
                    m_renderTarget->DrawLine({icx - 0.4f, icy + 2.2f}, {icx + 2.8f, icy - 1.4f}, brush, 1.45f, roundSt.Get());
                }
            }
            if (m_config.watermark.enabled) {
                setBrush(D2D1::ColorF(0.10f, 0.78f, 0.52f, 1.0f));
                m_renderTarget->FillEllipse(D2D1::Ellipse({brc.right - 7.0f, brc.top + 7.5f}, 2.6f, 2.6f), brush);
            }
        } else {
            m_renderTarget->DrawRoundedRectangle(
                D2D1::RoundedRect(D2D1::RectF(icx - 7.8f, icy - 5.8f, icx + 7.8f, icy + 5.8f), 2.5f, 2.5f), brush, 1.5f, roundSt.Get());
            m_renderTarget->DrawLine({icx - 4.5f, icy + 1.6f}, {icx + 4.5f, icy + 1.6f}, brush, 1.5f, roundSt.Get());
            m_renderTarget->DrawLine({icx - 4.5f, icy - 1.6f}, {icx + 1.5f, icy - 1.6f}, brush, 1.4f, roundSt.Get());
        }
        drawTxt(isZh ? tabShortZh[t] : tabShortEn[t], m_captionFmt.Get(),
                D2D1::RectF(brc.left, brc.bottom - 17.5f, brc.right, brc.bottom - 1.5f),
                active ? colPrimary : colSecondary, DWRITE_TEXT_ALIGNMENT_CENTER);
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
        setBrush(on ? colAccent : (dark ? D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.16f) : D2D1::ColorF(0.76f, 0.81f, 0.87f, 1.0f)));
        m_renderTarget->FillRoundedRectangle(D2D1::RoundedRect(rc, 9.5f, 9.5f), brush);
        const float knobX = on ? (rc.right - 9.5f) : (rc.left + 9.5f);
        setBrush(D2D1::ColorF(0.0f, 0.0f, 0.0f, 0.18f));
        m_renderTarget->FillEllipse(D2D1::Ellipse({knobX, topY + 10.4f}, 7.0f, 7.0f), brush);
        setBrush(D2D1::ColorF(1.0f, 1.0f, 1.0f, 1.0f));
        m_renderTarget->FillEllipse(D2D1::Ellipse({knobX, topY + 9.5f}, 6.8f, 6.8f), brush);
    };

    // 微晶陶瓷次级独立按钮 / 卡片
    auto drawOptionChip = [&](int hitId, const D2D1_RECT_F& rc, const std::wstring& label, bool active) {
        m_hitZones.push_back({hitId, rc, -1});
        const bool hov = (m_hoveredHitId == hitId);
        if (active) {
            setBrush(D2D1::ColorF(0.02f, 0.04f, 0.10f, dark ? 0.30f : 0.07f));
            m_renderTarget->FillRoundedRectangle(
                D2D1::RoundedRect(D2D1::RectF(rc.left, rc.top + 1.0f, rc.right, rc.bottom + 1.2f), 6.0f, 6.0f), brush);
            fillLinearVGradRounded(m_renderTarget.Get(), rc, 6.0f,
                dark ? D2D1::ColorF(accentRgb.r * 0.32f + 0.10f, accentRgb.g * 0.32f + 0.12f, accentRgb.b * 0.32f + 0.18f, 0.98f)
                     : D2D1::ColorF(1.0f, 1.0f, 1.0f, 1.0f),
                dark ? D2D1::ColorF(accentRgb.r * 0.24f + 0.08f, accentRgb.g * 0.24f + 0.10f, accentRgb.b * 0.24f + 0.15f, 0.98f)
                     : D2D1::ColorF(0.965f, 0.978f, 1.0f, 1.0f));
            setBrush(colAccent);
            m_renderTarget->DrawRoundedRectangle(D2D1::RoundedRect(rc, 6.0f, 6.0f), brush, 1.25f);
        } else {
            fillLinearVGradRounded(m_renderTarget.Get(), rc, 6.0f,
                dark ? D2D1::ColorF(0.125f, 0.152f, 0.215f, hov ? 0.95f : 0.75f)
                     : D2D1::ColorF(1.0f, 1.0f, 1.0f, hov ? 1.0f : 0.85f),
                dark ? D2D1::ColorF(0.102f, 0.125f, 0.182f, hov ? 0.95f : 0.75f)
                     : D2D1::ColorF(0.960f, 0.970f, 0.986f, hov ? 1.0f : 0.85f));
            setBrush(hov ? colAccent : colBorder);
            m_renderTarget->DrawRoundedRectangle(D2D1::RoundedRect(rc, 6.0f, 6.0f), brush, 0.9f);
        }
        drawTxt(label, m_captionFmt.Get(), rc, active ? (dark ? colPrimary : colAccent) : (hov ? colPrimary : colSecondary),
                DWRITE_TEXT_ALIGNMENT_CENTER);
    };

    // macOS Sequoia / Linear 级一体式凹槽分段控制器 (Unified Recessed Segmented Control)
    auto drawSegmentedGroup = [&](float topY, float height, int count,
                                  const int* hitIds, const wchar_t* const* labels, int activeIndex) {
        const D2D1_RECT_F trough = D2D1::RectF(padL, topY, padR, topY + height);
        setBrush(dark ? D2D1::ColorF(0.048f, 0.060f, 0.094f, 0.96f) : D2D1::ColorF(0.922f, 0.938f, 0.964f, 1.0f));
        m_renderTarget->FillRoundedRectangle(D2D1::RoundedRect(trough, 7.0f, 7.0f), brush);
        setBrush(colBorder);
        m_renderTarget->DrawRoundedRectangle(D2D1::RoundedRect(trough, 7.0f, 7.0f), brush, 0.8f);

        const float innerPad = 2.4f;
        const float cellW = (fullW - innerPad * 2.0f) / static_cast<float>((std::max)(1, count));
        for (int i = 0; i < count; ++i) {
            const D2D1_RECT_F crc = D2D1::RectF(
                trough.left + innerPad + i * cellW, trough.top + innerPad,
                trough.left + innerPad + (i + 1) * cellW, trough.bottom - innerPad);
            m_hitZones.push_back({hitIds[i], crc, i});
            const bool act = (i == activeIndex);
            if (act) {
                setBrush(D2D1::ColorF(0.02f, 0.04f, 0.10f, dark ? 0.38f : 0.09f));
                m_renderTarget->FillRoundedRectangle(
                    D2D1::RoundedRect(D2D1::RectF(crc.left, crc.top + 1.0f, crc.right, crc.bottom + 1.2f), 5.2f, 5.2f), brush);
                fillLinearVGradRounded(m_renderTarget.Get(), crc, 5.2f,
                    dark ? D2D1::ColorF(0.172f, 0.214f, 0.308f, 1.0f) : D2D1::ColorF(1.0f, 1.0f, 1.0f, 1.0f),
                    dark ? D2D1::ColorF(0.135f, 0.170f, 0.252f, 1.0f) : D2D1::ColorF(0.980f, 0.988f, 1.0f, 1.0f));
                setBrush(dark ? D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.14f) : D2D1::ColorF(0.06f, 0.10f, 0.20f, 0.085f));
                m_renderTarget->DrawRoundedRectangle(D2D1::RoundedRect(crc, 5.2f, 5.2f), brush, 0.85f);
            } else if (m_hoveredHitId == hitIds[i]) {
                setBrush(dark ? D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.045f) : D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.45f));
                m_renderTarget->FillRoundedRectangle(D2D1::RoundedRect(crc, 5.2f, 5.2f), brush);
            }
            drawTxt(labels[i], m_captionFmt.Get(), crc, act ? colPrimary : colSecondary, DWRITE_TEXT_ALIGNMENT_CENTER);
        }
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
            m_renderTarget->FillEllipse(D2D1::Ellipse({dcx, dcy}, 5.5f, 5.5f), brush);
            setBrush(colBorder);
            m_renderTarget->DrawEllipse(D2D1::Ellipse({dcx, dcy}, 6.6f, 6.6f), brush, 1.1f);
            trackL = padL + 68.0f;
        }
        const D2D1_RECT_F trk = D2D1::RectF(trackL, topY + 8.2f, padR - 48.0f, topY + 11.8f);
        m_sliderTrackRects[static_cast<size_t>(kind)] = trk;
        m_hitZones.push_back({hitId, D2D1::RectF(trk.left - 5.0f, topY, trk.right + 5.0f, topY + 20.0f), -1});

        setBrush(dark ? D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.10f) : D2D1::ColorF(0.06f, 0.10f, 0.18f, 0.085f));
        m_renderTarget->FillRoundedRectangle(D2D1::RoundedRect(trk, 1.8f, 1.8f), brush);
        const float clamped = std::clamp(norm01, 0.0f, 1.0f);
        const float kx = trk.left + (trk.right - trk.left) * clamped;
        setBrush(colAccent);
        m_renderTarget->FillRoundedRectangle(
            D2D1::RoundedRect(D2D1::RectF(trk.left, trk.top, (std::max)(trk.left + 1.8f, kx), trk.bottom), 1.8f, 1.8f), brush);

        // 双层微投影白瓷圆珠旋钮 (White Porcelain Thumb)
        const float ky = (trk.top + trk.bottom) * 0.5f;
        setBrush(D2D1::ColorF(0.02f, 0.04f, 0.10f, dark ? 0.36f : 0.18f));
        m_renderTarget->FillEllipse(D2D1::Ellipse({kx, ky + 1.0f}, 6.0f, 6.0f), brush);
        setBrush(D2D1::ColorF(1.0f, 1.0f, 1.0f, 1.0f));
        m_renderTarget->FillEllipse(D2D1::Ellipse({kx, ky}, 5.5f, 5.5f), brush);
        setBrush(m_activeSlider == kind ? colAccent : (dark ? D2D1::ColorF(1, 1, 1, 0.45f) : D2D1::ColorF(0.10f, 0.16f, 0.28f, 0.24f)));
        m_renderTarget->DrawEllipse(D2D1::Ellipse({kx, ky}, 5.5f, 5.5f), brush, m_activeSlider == kind ? 1.5f : 1.0f);

        drawTxt(valStr, m_monoFmt.Get(), D2D1::RectF(padR - 44.0f, topY, padR, topY + 20.0f),
                colPrimary, DWRITE_TEXT_ALIGNMENT_TRAILING);
    };

    if (m_activeTab == LeftRailTab::Annotations) {
        renderAnnotationDrawer(padL, padR, curY, drawerRc.bottom - 10.0f, dark, isZh, brush);
        return;
    }

    if (m_activeTab == LeftRailTab::Watermark) {
        renderWatermarkDrawer(padL, padR, curY, drawerRc.bottom - 10.0f, dark, isZh, brush);
        return;
    }

    if (m_activeTab == LeftRailTab::CameraCursor) {
        drawTxt(isZh ? L"鼠标自动运镜" : L"Auto Zoom", m_sectionFmt.Get(),
                D2D1::RectF(padL, curY, padR, curY + 20.0f), colPrimary);
        curY += 23.0f;
        {
            const int ids[3] = { Hit_ZoomMode_None, Hit_ZoomMode_2D, Hit_ZoomMode_3D };
            static const wchar_t* lblZh[3] = { L"关闭", L"2D 运镜", L"3D 运镜" };
            static const wchar_t* lblEn[3] = { L"Off", L"2D Zoom", L"3D Motion" };
            const int actIdx = (!m_config.autoZoomEnabled || m_config.zoomMode == StudioZoomMode::None)
                ? 0 : (m_config.zoomMode == StudioZoomMode::Motion3D ? 2 : 1);
            drawSegmentedGroup(curY, 27.0f, 3, ids, isZh ? lblZh : lblEn, actIdx);
        }
        curY += 35.0f;

        drawTxt(isZh ? L"默认放大倍率" : L"Zoom Factor", m_sectionFmt.Get(),
                D2D1::RectF(padL, curY, padR, curY + 20.0f), colPrimary);
        curY += 23.0f;
        {
            const int ids[3] = { Hit_ZoomPreset_150, Hit_ZoomPreset_185, Hit_ZoomPreset_220 };
            static const wchar_t* lblZh[3] = { L"温和 1.5x", L"标准 1.85x", L"特写 2.2x" };
            static const wchar_t* lblEn[3] = { L"1.5x", L"1.85x", L"2.2x" };
            const int actIdx = (std::abs(m_config.defaultZoomFactor - 1.50f) < 0.12f)
                ? 0 : ((std::abs(m_config.defaultZoomFactor - 2.20f) < 0.15f) ? 2 : 1);
            drawSegmentedGroup(curY, 26.0f, 3, ids, isZh ? lblZh : lblEn, actIdx);
        }
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
        curY += 25.0f;
        {
            const int ids[3] = { Hit_CursorScale_100, Hit_CursorScale_135, Hit_CursorScale_170 };
            static const wchar_t* lbls[3] = { L"1.0x", L"1.35x", L"1.7x" };
            const int actIdx = !m_config.smoothCursorEnabled ? -1
                : (std::abs(m_config.cursorScale - 1.00f) < 0.15f ? 0
                   : (std::abs(m_config.cursorScale - 1.70f) < 0.15f ? 2 : 1));
            drawSegmentedGroup(curY, 26.0f, 3, ids, lbls, actIdx);
        }
        curY += 34.0f;

        drawTxt(isZh ? L"光标聚光环跟随" : L"Cursor Highlight Halo", m_bodyFmt.Get(),
                D2D1::RectF(padL, curY, padR - 44.0f, curY + 20.0f), colPrimary);
        drawTogglePill(Hit_ToggleCursorHalo, padR, curY, m_config.cursorHighlightHalo);
        curY += 28.0f;

        setBrush(colBorder);
        m_renderTarget->DrawLine({padL, curY}, {padR, curY}, brush, 1.0f);
        curY += 12.0f;

        drawTxt(isZh ? L"鼠标点击动效" : L"Click Effect", m_sectionFmt.Get(),
                D2D1::RectF(padL, curY, padR, curY + 20.0f), colPrimary);
        curY += 24.0f;
        {
            const float w2 = (fullW - 6.0f) * 0.5f;
            const StudioClickEffectStyle effStyle = m_config.showClickRipple
                ? m_config.clickEffectStyle
                : StudioClickEffectStyle::TactileOnly;
            drawOptionChip(Hit_ClickStyleBase + 0,
                           D2D1::RectF(padL, curY, padL + w2, curY + 26.0f),
                           isZh ? L"微晶单波 (默认)" : L"Crystal (Def)",
                           effStyle == StudioClickEffectStyle::CrystalPulse);
            drawOptionChip(Hit_ClickStyleBase + 1,
                           D2D1::RectF(padL + w2 + 6.0f, curY, padR, curY + 26.0f),
                           isZh ? L"柔雾触点" : L"Soft Touch",
                           effStyle == StudioClickEffectStyle::SoftTouch);
            curY += 32.0f;
            drawOptionChip(Hit_ClickStyleBase + 2,
                           D2D1::RectF(padL, curY, padL + w2, curY + 26.0f),
                           isZh ? L"错峰涟漪" : L"Dual Ripple",
                           effStyle == StudioClickEffectStyle::DualRipple);
            drawOptionChip(Hit_ClickStyleBase + 3,
                           D2D1::RectF(padL + w2 + 6.0f, curY, padR, curY + 26.0f),
                           isZh ? L"仅物理按压" : L"Tactile Only",
                           effStyle == StudioClickEffectStyle::TactileOnly);
            curY += 32.0f;
        }
        drawTxt(isZh ? L"点击上方卡片可在右侧舞台实时预览光标轻叩与光学波纹反馈。"
                     : L"Click any style above to preview live cursor press & optical pulse.",
                m_captionWrapFmt.Get(), D2D1::RectF(padL, curY, padR, curY + 38.0f), colSecondary);
        return;
    }

    if (m_activeTab == LeftRailTab::Captions) {
        drawTxt(isZh ? L"离线语音字幕对齐" : L"Offline Auto Captions", m_sectionFmt.Get(),
                D2D1::RectF(padL, curY, padR - 44.0f, curY + 20.0f), colPrimary);
        drawTogglePill(Hit_ToggleCaptions, padR, curY, m_config.autoCaptionsEnabled);
        curY += 30.0f;
        drawTxt(isZh ? L"开启麦克风录制时，自动识别语音生成底部字幕并导出同名 .srt 文件。"
                     : L"Transcribes mic audio into synced bottom captions and .srt file.",
                m_captionWrapFmt.Get(), D2D1::RectF(padL, curY, padR, curY + 46.0f), colSecondary);
        return;
    }

    drawTxt(isZh ? L"视频尺寸" : L"Video Size", m_sectionFmt.Get(), D2D1::RectF(padL, curY, padR, curY + 19.0f), colPrimary);
    curY += 22.0f;
    {
        const int aspIds[5] = { Hit_AspectBase + 0, Hit_AspectBase + 1, Hit_AspectBase + 2, Hit_AspectBase + 3, Hit_AspectBase + 4 };
        static const wchar_t* aspZh[5] = { L"原始", L"16:9", L"9:16", L"4:3", L"1:1" };
        static const wchar_t* aspEn[5] = { L"Orig", L"16:9", L"9:16", L"4:3", L"1:1" };
        drawSegmentedGroup(curY, 25.0f, 5, aspIds, isZh ? aspZh : aspEn, static_cast<int>(m_config.aspectRatio));
    }
    curY += 30.0f;

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
        const int catIds[5] = { Hit_WallCatBase + 0, Hit_WallCatBase + 1, Hit_WallCatBase + 2, Hit_WallCatBase + 3, Hit_WallCatBase + 4 };
        static const wchar_t* catZh[5] = { L"默认", L"沉稳", L"轻盈", L"风景", L"自定义" };
        static const wchar_t* catEn[5] = { L"All", L"Dark", L"Light", L"Scene", L"Custom" };
        drawSegmentedGroup(curY, 24.0f, 5, catIds, isZh ? catZh : catEn, m_wallpaperCategory);
    }
    curY += 29.0f;

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
        if (!fmt || txt.empty() || r.right <= r.left || r.bottom <= r.top) return;
        fmt->SetTextAlignment(hAlign);
        fmt->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
        setBrush(col);
        m_renderTarget->DrawText(txt.c_str(), static_cast<UINT32>(txt.size()), fmt, r, brush, D2D1_DRAW_TEXT_OPTIONS_CLIP);
    };

    const bool exporting = m_exporting.load(std::memory_order_acquire);

    const D2D1_RECT_F fullRc = D2D1::RectF(0.0f, 0.0f, W, H);
    if (!exporting) {
        m_hitZones.push_back({Hit_ModalBackdrop, fullRc, -1});
    }
    setBrush(D2D1::ColorF(0.02f, 0.03f, 0.06f, dark ? 0.68f : 0.45f));
    m_renderTarget->FillRectangle(fullRc, brush);

    const float cardW = 556.0f;
    const float cardH = (!exporting && m_exportCompletedCard) ? 350.0f : 468.0f;
    const float cardL = (W - cardW) * 0.5f;
    const float cardT = (H - cardH) * 0.5f;
    const D2D1_RECT_F cardRc = D2D1::RectF(cardL, cardT, cardL + cardW, cardT + cardH);
    m_hitZones.push_back({Hit_ModalCardBody, cardRc, -1});

    setBrush(colModalBg);
    m_renderTarget->FillRoundedRectangle(D2D1::RoundedRect(cardRc, 16.0f, 16.0f), brush);
    setBrush(colBorder);
    m_renderTarget->DrawRoundedRectangle(D2D1::RoundedRect(cardRc, 16.0f, 16.0f), brush, 1.2f);

    const D2D1_RECT_F mCloseRc = D2D1::RectF(cardRc.right - 40.0f, cardT + 14.0f, cardRc.right - 14.0f, cardT + 40.0f);
    if (!exporting) {
        m_hitZones.push_back({Hit_ModalCloseBtn, mCloseRc, -1});
    }
    setBrush(m_hoveredHitId == Hit_ModalCloseBtn ? D2D1::ColorF(0.94f, 0.27f, 0.27f, 1.0f) : colSecondary);
    const float mcx = (mCloseRc.left + mCloseRc.right) * 0.5f;
    const float mcy = (mCloseRc.top + mCloseRc.bottom) * 0.5f;
    m_renderTarget->DrawLine({mcx - 4.5f, mcy - 4.5f}, {mcx + 4.5f, mcy + 4.5f}, brush, 1.6f);
    m_renderTarget->DrawLine({mcx + 4.5f, mcy - 4.5f}, {mcx - 4.5f, mcy + 4.5f}, brush, 1.6f);

    if (!exporting && m_exportCompletedCard) {
        const float inL = cardL + 26.0f;
        const float inR = cardRc.right - 26.0f;

        // 顶部翠绿矢量对勾圆徽 + 导出成功标题
        const D2D1_POINT_2F badgePt{cardL + cardW * 0.5f - 54.0f, cardT + 31.0f};
        setBrush(D2D1::ColorF(0.13f, 0.77f, 0.44f, dark ? 0.20f : 0.14f));
        m_renderTarget->FillEllipse(D2D1::Ellipse(badgePt, 11.5f, 11.5f), brush);
        setBrush(D2D1::ColorF(0.13f, 0.77f, 0.44f, 1.0f));
        m_renderTarget->DrawLine({badgePt.x - 4.4f, badgePt.y + 0.2f}, {badgePt.x - 1.2f, badgePt.y + 3.6f}, brush, 2.0f);
        m_renderTarget->DrawLine({badgePt.x - 1.2f, badgePt.y + 3.6f}, {badgePt.x + 5.0f, badgePt.y - 3.2f}, brush, 2.0f);

        drawTxt(isZh ? L"导出完成" : L"Export Completed", m_titleFmt.Get(),
                D2D1::RectF(badgePt.x + 17.0f, cardT + 14.0f, cardRc.right - 46.0f, cardT + 48.0f),
                colPrimary, DWRITE_TEXT_ALIGNMENT_LEADING);

        // 中部真实落盘文件指标微晶信息舱
        const std::filesystem::path expFs(tools3000::core::WinUtils::utf8ToWstring(m_lastExportedFilePath));
        const std::wstring fileNameW = expFs.filename().wstring();
        const std::wstring parentDirW = expFs.parent_path().wstring();
        const std::wstring sizeW = formatEstimatedSize(m_lastExportedFileBytes);

        const D2D1_RECT_F infoBox = D2D1::RectF(inL, cardT + 58.0f, inR, cardT + 150.0f);
        setBrush(colFieldBg);
        m_renderTarget->FillRoundedRectangle(D2D1::RoundedRect(infoBox, 10.0f, 10.0f), brush);
        setBrush(colBorder);
        m_renderTarget->DrawRoundedRectangle(D2D1::RoundedRect(infoBox, 10.0f, 10.0f), brush, 1.0f);

        // 体积微晶胶囊徽章
        const D2D1_RECT_F sizePill = D2D1::RectF(infoBox.right - 92.0f, infoBox.top + 11.0f, infoBox.right - 12.0f, infoBox.top + 35.0f);
        setBrush(D2D1::ColorF(accentRgb.r, accentRgb.g, accentRgb.b, dark ? 0.22f : 0.12f));
        m_renderTarget->FillRoundedRectangle(D2D1::RoundedRect(sizePill, 6.0f, 6.0f), brush);
        drawTxt(sizeW, m_monoFmt.Get(), sizePill, colAccent, DWRITE_TEXT_ALIGNMENT_CENTER);

        drawTxt(fileNameW, m_sectionFmt.Get(),
                D2D1::RectF(infoBox.left + 14.0f, infoBox.top + 10.0f, sizePill.left - 8.0f, infoBox.top + 36.0f),
                colPrimary);
        drawTxt(m_lastExportedSpecLine, m_bodyFmt.Get(),
                D2D1::RectF(infoBox.left + 14.0f, infoBox.top + 39.0f, infoBox.right - 14.0f, infoBox.top + 63.0f),
                colSecondary);
        drawTxt(parentDirW, m_captionFmt.Get(),
                D2D1::RectF(infoBox.left + 14.0f, infoBox.top + 64.0f, infoBox.right - 14.0f, infoBox.top + 86.0f),
                colSecondary);

        // 快捷文件操作栏：复制文件 / 直接打开预览 / 打开所在文件夹
        const float actTop = cardT + 166.0f;
        const float actH = 38.0f;
        const float actGap = 10.0f;
        const float actW = (inR - inL - actGap * 2.0f) / 3.0f;
        const bool copiedNow = (GetTickCount64() < m_copyFeedbackUntilTick);

        auto drawActionChip = [&](int hitId, int idx, const std::wstring& label, bool highlightGreen = false) {
            const float bx = inL + static_cast<float>(idx) * (actW + actGap);
            const D2D1_RECT_F brc = D2D1::RectF(bx, actTop, bx + actW, actTop + actH);
            m_hitZones.push_back({hitId, brc, -1});
            const bool hov = (m_hoveredHitId == hitId);
            if (highlightGreen) {
                setBrush(D2D1::ColorF(0.13f, 0.77f, 0.44f, dark ? 0.24f : 0.16f));
            } else if (hov) {
                setBrush(D2D1::ColorF(accentRgb.r, accentRgb.g, accentRgb.b, dark ? 0.22f : 0.12f));
            } else {
                setBrush(colFieldBg);
            }
            m_renderTarget->FillRoundedRectangle(D2D1::RoundedRect(brc, 8.0f, 8.0f), brush);
            setBrush(highlightGreen ? D2D1::ColorF(0.13f, 0.77f, 0.44f, 0.85f) : (hov ? colAccent : colBorder));
            m_renderTarget->DrawRoundedRectangle(D2D1::RoundedRect(brc, 8.0f, 8.0f), brush, 1.0f);
            drawTxt(label, m_bodyFmt.Get(), brc,
                    highlightGreen ? D2D1::ColorF(0.16f, 0.82f, 0.48f, 1.0f) : colPrimary,
                    DWRITE_TEXT_ALIGNMENT_CENTER);
        };

        drawActionChip(
            Hit_ModalResultCopyFileBtn, 0,
            copiedNow ? (isZh ? L"已复制到剪贴板" : L"Copied") : (isZh ? L"复制文件 (Ctrl+C)" : L"Copy File"),
            copiedNow);
        drawActionChip(Hit_ModalResultOpenPlayBtn, 1, isZh ? L"打开预览" : L"Open File");
        drawActionChip(Hit_ModalResultRevealBtn, 2, isZh ? L"打开所在位置" : L"Show in Folder");

        // 提示小字：告知可随时点遮罩回画布微调，或继续导出其他格式对比（支持双行折行，零溢出）
        drawTxt(isZh ? L"演播室母带保持无损就绪：可继续导出 GIF / 其他参数对比，或点击外侧遮罩回画布微调"
                     : L"Master recording stays intact: export another format (e.g. GIF) or click backdrop to tweak canvas",
                m_captionWrapFmt.Get(),
                D2D1::RectF(inL, cardT + 214.0f, inR, cardT + 250.0f),
                colSecondary, DWRITE_TEXT_ALIGNMENT_CENTER);

        // 底部主次双决策按钮：[继续导出其他格式]  |  [完成并关闭]
        const float botTop = cardT + 262.0f;
        const float botH = 40.0f;
        const float botGap = 12.0f;
        const float botHalfW = (inR - inL - botGap) * 0.5f;
        const D2D1_RECT_F moreBtnRc = D2D1::RectF(inL, botTop, inL + botHalfW, botTop + botH);
        const D2D1_RECT_F doneBtnRc = D2D1::RectF(inL + botHalfW + botGap, botTop, inR, botTop + botH);
        m_hitZones.push_back({Hit_ModalResultExportMoreBtn, moreBtnRc, -1});
        m_hitZones.push_back({Hit_ModalResultFinishCloseBtn, doneBtnRc, -1});

        setBrush(m_hoveredHitId == Hit_ModalResultExportMoreBtn
            ? D2D1::ColorF(accentRgb.r, accentRgb.g, accentRgb.b, dark ? 0.20f : 0.12f)
            : colFieldBg);
        m_renderTarget->FillRoundedRectangle(D2D1::RoundedRect(moreBtnRc, 8.0f, 8.0f), brush);
        setBrush(m_hoveredHitId == Hit_ModalResultExportMoreBtn ? colAccent : colBorder);
        m_renderTarget->DrawRoundedRectangle(D2D1::RoundedRect(moreBtnRc, 8.0f, 8.0f), brush, 1.2f);
        drawTxt(isZh ? L"继续导出其他格式" : L"Export Another Format",
                m_sectionFmt.Get(), moreBtnRc, colPrimary, DWRITE_TEXT_ALIGNMENT_CENTER);

        setBrush(m_hoveredHitId == Hit_ModalResultFinishCloseBtn
            ? D2D1::ColorF((std::min)(1.0f, accentRgb.r + 0.07f), (std::min)(1.0f, accentRgb.g + 0.07f), (std::min)(1.0f, accentRgb.b + 0.07f), 1.0f)
            : colAccent);
        m_renderTarget->FillRoundedRectangle(D2D1::RoundedRect(doneBtnRc, 8.0f, 8.0f), brush);
        drawTxt(isZh ? L"完成并关闭 (Enter)" : L"Done & Close (Enter)",
                m_sectionFmt.Get(), doneBtnRc, D2D1::ColorF(1.0f, 1.0f, 1.0f, 1.0f), DWRITE_TEXT_ALIGNMENT_CENTER);
        return;
    }

    drawTxt(isZh ? L"导出" : L"Export", m_titleFmt.Get(),
            D2D1::RectF(cardL + 24.0f, cardT + 14.0f, cardRc.right - 24.0f, cardT + 48.0f),
            colPrimary, DWRITE_TEXT_ALIGNMENT_CENTER);

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

    y += 66.0f;
    {
        std::wstring fmtTip;
        if (m_exportOpts.format == StudioExportFormat::GIF_WeChat) {
            fmtTip = isZh
                ? L"提示：<=800px 微信按「动态表情」气泡内免点自动播放（不支持双击放大）；如需在微信双击打开独立查看器滚轮放大，请选「GIF 高清大图」或 MP4。"
                : L"Note: <=800px auto-plays inline in WeChat as a Sticker (no zoom viewer). Choose 'GIF (HD · Zoomable)' or MP4 to enable double-click viewer zoom.";
        } else if (m_exportOpts.format == StudioExportFormat::GIF_HD) {
            fmtTip = isZh
                ? L"提示：长边 >=1080px 发微信按「普通高清动图」展示，支持双击打开微信独立查看器循环播放并滚轮无级放大看清细节。"
                : L"Note: >=1080px sends as an HD animated image in WeChat — supports double-click viewer playback and scroll-wheel zoom.";
        } else if (m_exportOpts.format == StudioExportFormat::MP4_H264 ||
                   m_exportOpts.format == StudioExportFormat::MP4_HEVC) {
            fmtTip = isZh
                ? L"提示：完整保留立体声音轨与 24-bit 真彩锐利画质，微信/飞书/网页均支持独立大窗高清全屏放大播放。"
                : L"Note: Preserves stereo audio & 24-bit crisp color with full-screen zoom playback across WeChat, Lark & web.";
        } else {
            fmtTip = isZh
                ? L"提示：现代网页 24-bit 真彩动图/视频容器，无 256 色限制，适合嵌入文档、GitHub 与技术博客。"
                : L"Note: 24-bit true-color web media container without 256-color limits, ideal for docs, GitHub & blogs.";
        }
        const D2D1_RECT_F tipBox = D2D1::RectF(inL, y, inR, y + 42.0f);
        setBrush(dark ? D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.045f) : D2D1::ColorF(0.06f, 0.09f, 0.16f, 0.038f));
        m_renderTarget->FillRoundedRectangle(D2D1::RoundedRect(tipBox, 7.0f, 7.0f), brush);
        drawTxt(fmtTip, m_captionWrapFmt.Get(),
                D2D1::RectF(tipBox.left + 10.0f, tipBox.top + 3.0f, tipBox.right - 10.0f, tipBox.bottom - 3.0f),
                colSecondary);
    }

    y += 50.0f;
    auto drawCompactCheckbox = [&](int hitId, float cxLeft, float cxRight, bool checked, const std::wstring& label) {
        const D2D1_RECT_F hitRc = D2D1::RectF(cxLeft, y, cxRight, y + 36.0f);
        if (!exporting) {
            m_hitZones.push_back({hitId, hitRc, -1});
        }
        const D2D1_RECT_F boxRc = D2D1::RectF(cxLeft, y + 9.0f, cxLeft + 17.0f, y + 26.0f);
        if (checked) {
            setBrush(colAccent);
            m_renderTarget->FillRoundedRectangle(D2D1::RoundedRect(boxRc, 4.5f, 4.5f), brush);
            setBrush(D2D1::ColorF(1.0f, 1.0f, 1.0f, 1.0f));
            m_renderTarget->DrawLine({boxRc.left + 3.8f, boxRc.top + 8.8f}, {boxRc.left + 7.4f, boxRc.top + 12.4f}, brush, 1.85f);
            m_renderTarget->DrawLine({boxRc.left + 7.4f, boxRc.top + 12.4f}, {boxRc.left + 13.6f, boxRc.top + 4.8f}, brush, 1.85f);
        } else {
            setBrush(colFieldBg);
            m_renderTarget->FillRoundedRectangle(D2D1::RoundedRect(boxRc, 4.5f, 4.5f), brush);
            setBrush(m_hoveredHitId == hitId ? colAccent : colBorder);
            m_renderTarget->DrawRoundedRectangle(D2D1::RoundedRect(boxRc, 4.5f, 4.5f), brush, 1.2f);
        }
        drawTxt(label, m_captionFmt.Get(),
                D2D1::RectF(boxRc.right + 6.5f, y, cxRight, y + 36.0f), colSecondary);
    };

    drawCompactCheckbox(Hit_ModalSavePrefsCheck, inL, inL + 112.0f, m_saveExportPrefs,
                        isZh ? L"保存偏好" : L"Save Prefs");
    drawCompactCheckbox(Hit_ModalAutoCloseCheck, inL + 118.0f, inL + 248.0f, m_autoCloseAfterExport,
                        isZh ? L"导完自动关闭" : L"Auto-Close");

    const cv::Size targetCanvasSz = StudioVideoCompositor::resolveOutputCanvasSize(
        m_origWidth, m_origHeight, m_config.aspectRatio);
    const uint64_t estBytes = StudioVideoCompositor::estimateExportSizeBytes(
        targetCanvasSz.width, targetCanvasSz.height, computeEffectiveDurationSec(), m_exportOpts);
    const std::wstring estStr = formatEstimatedSize(estBytes);
    const D2D1_RECT_F expBtn = D2D1::RectF(inR - 198.0f, y - 1.0f, inR, y + 37.0f);
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
                StudioExportFormat::GIF_HD, StudioExportFormat::GIF_WeChat,
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

namespace {

struct CoachCinemaFonts {
    IDWriteTextFormat* sectionFmt = nullptr;
    IDWriteTextFormat* captionFmt = nullptr;
    IDWriteTextFormat* captionWrapFmt = nullptr;
    IDWriteTextFormat* monoFmt = nullptr;
};

void renderLuxuryCoachCardScene(
    ID2D1Factory* d2dFactory,
    ID2D1RenderTarget* rt,
    ID2D1SolidColorBrush* brush,
    const CoachCinemaFonts& fonts,
    int coachHitId,
    bool targetEnabled,
    const D2D1_RECT_F& cardRc,
    float anchorTipX,
    float phase,
    float fadeAlpha,
    bool dark,
    bool isZh,
    float accentR, float accentG, float accentB) {
    if (!d2dFactory || !rt || !brush) return;

    auto rgba = [&](float r, float g, float b, float a) -> D2D1_COLOR_F {
        return D2D1::ColorF(r, g, b, std::clamp(a * fadeAlpha, 0.0f, 1.0f));
    };
    auto setBrush = [&](const D2D1_COLOR_F& c) { brush->SetColor(c); };
    auto smooth01 = [](float x) noexcept -> float {
        x = std::clamp(x, 0.0f, 1.0f);
        return x * x * (3.0f - 2.0f * x);
    };
    auto easeOutCubic = [](float x) noexcept -> float {
        x = std::clamp(x, 0.0f, 1.0f);
        const float inv = 1.0f - x;
        return 1.0f - inv * inv * inv;
    };
    auto spring01 = [](float x) noexcept -> float {
        x = std::clamp(x, 0.0f, 1.0f);
        if (x >= 0.999f) return 1.0f;
        return 1.0f - std::exp(-5.6f * x) * std::cos(4.8f * x);
    };

    ComPtr<ID2D1StrokeStyle> roundStroke;
    {
        D2D1_STROKE_STYLE_PROPERTIES sp = D2D1::StrokeStyleProperties(
            D2D1_CAP_STYLE_ROUND, D2D1_CAP_STYLE_ROUND, D2D1_CAP_STYLE_ROUND, D2D1_LINE_JOIN_ROUND);
        d2dFactory->CreateStrokeStyle(sp, nullptr, 0, roundStroke.GetAddressOf());
    }

    auto fillVGradRounded = [&](const D2D1_RECT_F& rc, float rad, const D2D1_COLOR_F& cTop, const D2D1_COLOR_F& cBot) {
        if (rc.right <= rc.left || rc.bottom <= rc.top) return;
        D2D1_GRADIENT_STOP stops[2] = { {0.0f, cTop}, {1.0f, cBot} };
        ComPtr<ID2D1GradientStopCollection> sc;
        if (SUCCEEDED(rt->CreateGradientStopCollection(stops, 2, D2D1_GAMMA_2_2, D2D1_EXTEND_MODE_CLAMP, sc.GetAddressOf())) && sc) {
            ComPtr<ID2D1LinearGradientBrush> gb;
            const auto props = D2D1::LinearGradientBrushProperties({rc.left, rc.top}, {rc.left, rc.bottom});
            if (SUCCEEDED(rt->CreateLinearGradientBrush(props, sc.Get(), gb.GetAddressOf())) && gb) {
                rt->FillRoundedRectangle(D2D1::RoundedRect(rc, rad, rad), gb.Get());
                return;
            }
        }
        setBrush(cTop);
        rt->FillRoundedRectangle(D2D1::RoundedRect(rc, rad, rad), brush);
    };

    auto fillRadialGlow = [&](float cx, float cy, float rx, float ry, const D2D1_COLOR_F& centerCol) {
        if (rx <= 0.5f || ry <= 0.5f) return;
        D2D1_GRADIENT_STOP stops[3] = {
            {0.0f, centerCol},
            {0.55f, D2D1::ColorF(centerCol.r, centerCol.g, centerCol.b, centerCol.a * 0.35f)},
            {1.0f, D2D1::ColorF(centerCol.r, centerCol.g, centerCol.b, 0.0f)}
        };
        ComPtr<ID2D1GradientStopCollection> sc;
        if (SUCCEEDED(rt->CreateGradientStopCollection(stops, 3, D2D1_GAMMA_2_2, D2D1_EXTEND_MODE_CLAMP, sc.GetAddressOf())) && sc) {
            ComPtr<ID2D1RadialGradientBrush> rb;
            const auto props = D2D1::RadialGradientBrushProperties({cx, cy}, {0.0f, 0.0f}, rx, ry);
            if (SUCCEEDED(rt->CreateRadialGradientBrush(props, sc.Get(), rb.GetAddressOf())) && rb) {
                rt->FillEllipse(D2D1::Ellipse({cx, cy}, rx, ry), rb.Get());
            }
        }
    };

    auto drawTextClip = [&](const std::wstring& txt, IDWriteTextFormat* fmt, const D2D1_RECT_F& r,
                            const D2D1_COLOR_F& col, DWRITE_TEXT_ALIGNMENT ha = DWRITE_TEXT_ALIGNMENT_LEADING) {
        if (!fmt || txt.empty() || r.right <= r.left || r.bottom <= r.top) return;
        fmt->SetTextAlignment(ha);
        fmt->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
        setBrush(col);
        rt->DrawText(txt.c_str(), static_cast<UINT32>(txt.size()), fmt, r, brush, D2D1_DRAW_TEXT_OPTIONS_CLIP);
    };

    std::wstring titleTxt;
    std::wstring shortcutTxt;
    std::wstring descTxt;

    switch (coachHitId) {
        case Hit_SplitAtPlayhead:
            titleTxt = isZh ? L"在指针处拆分片段" : L"Split Clip at Playhead";
            shortcutTxt = L"S";
            descTxt = isZh
                ? L"将当前视频从播放指针处切开一分为二。在需要剪掉的首尾位置各切一刀，即可分离出中间片段。"
                : L"Splits the clip into two at the playhead. Cut at start and end to isolate unwanted sections.";
            break;
        case Hit_DeleteSelectedItem:
            titleTxt = isZh ? L"删除选中项并自动拼接" : L"Ripple Delete";
            shortcutTxt = L"Del";
            descTxt = targetEnabled
                ? (isZh ? L"删除当前选中的视频片段、运镜或标注；删除片段后右侧画面自动向左无缝贴合，不留黑屏。"
                        : L"Removes the selected clip, zoom, or mark and snaps subsequent clips together seamlessly.")
                : (isZh ? L"先用「拆分(S)」切出片段并点击选中（或点选运镜/标注块），按 Del 即可删除并自动无缝拼接。"
                        : L"Split a clip with 'S' and click to select it first, then press Del to remove and snap.");
            break;
        case Hit_CycleClipSpeed:
            titleTxt = isZh ? L"片段局部变速" : L"Clip Playback Speed";
            shortcutTxt = L"1.0x ~ 3.0x";
            descTxt = isZh
                ? L"为选中的单个片段循环切换 1.0x / 1.2x / 1.5x / 2.0x / 3.0x 倍速，轻松快进冗长的加载等待。"
                : L"Cycles playback speed (1.0x to 3.0x) on the selected clip to fast-forward long waits.";
            break;
        case Hit_UndoBtn:
            titleTxt = isZh ? L"撤销上一步操作" : L"Undo Last Action";
            shortcutTxt = L"Ctrl + Z";
            descTxt = isZh
                ? L"支持最多 40 步无损回退，随时撤回刚才的片段拆分、删除、变速、运镜或标注修改。"
                : L"Reverts up to 40 steps of clip splits, deletions, speed changes, zooms, or annotations.";
            break;
        case Hit_RedoBtn:
            titleTxt = isZh ? L"重做已撤销操作" : L"Redo Action";
            shortcutTxt = L"Ctrl + Y";
            descTxt = isZh
                ? L"恢复刚刚撤销的剪辑、变速、运镜或画面标注状态。"
                : L"Restores the most recently undone timeline edit, camera zoom, or annotation.";
            break;
        case Hit_QuickOpenAnnotations:
            titleTxt = isZh ? L"画面重点标注" : L"Timeline Annotations";
            shortcutTxt = isZh ? L"19 种样式" : L"19 Styles";
            descTxt = isZh
                ? L"在指定时间段添加手绘重点圈、圆角框、箭头、步骤序号、聚光灯、局部放大镜或马赛克打码。"
                : L"Places timed marker circles, boxes, arrows, step badges, spotlight, magnifier, or blur.";
            break;
        case Hit_AddZoomAtPlayhead:
            titleTxt = isZh ? L"插入镜头推近特写" : L"Insert Camera Zoom";
            shortcutTxt = isZh ? L"可拖焦点" : L"Drag Focus";
            descTxt = isZh
                ? L"在指针处新增一段平滑放大运镜；选中蓝色运镜块后，可在画面直接拖动十字准星调整聚焦位置。"
                : L"Adds a smooth zoom-in segment at playhead; drag the stage crosshair to re-aim focus.";
            break;
        default:
            return;
    }

    // 1. 四层渐进云雾悬浮弥散阴影
    for (int s = 3; s >= 0; --s) {
        const float spread = 1.2f + s * 2.2f;
        const float yOff = 2.0f + s * 2.4f;
        const float shAlpha = (dark ? 0.16f : 0.042f) * (1.0f - s * 0.18f);
        setBrush(rgba(0.03f, 0.06f, 0.13f, shAlpha));
        rt->FillRoundedRectangle(
            D2D1::RoundedRect(D2D1::RectF(cardRc.left - spread, cardRc.top + yOff - spread * 0.3f,
                                          cardRc.right + spread, cardRc.bottom + yOff + spread * 0.5f),
                              15.0f + spread, 15.0f + spread),
            brush);
    }

    // 2. 丝绸渐变画廊外壳 + 底部指向圆润导角
    const D2D1_COLOR_F shellTop = dark ? rgba(0.098f, 0.125f, 0.192f, 0.99f) : rgba(1.000f, 1.000f, 1.000f, 0.995f);
    const D2D1_COLOR_F shellBot = dark ? rgba(0.068f, 0.088f, 0.140f, 0.99f) : rgba(0.965f, 0.976f, 0.992f, 0.995f);
    fillVGradRounded(cardRc, 14.5f, shellTop, shellBot);

    const float tipX = std::clamp(anchorTipX, cardRc.left + 24.0f, cardRc.right - 24.0f);
    {
        ComPtr<ID2D1PathGeometry> caretGeo;
        if (SUCCEEDED(d2dFactory->CreatePathGeometry(&caretGeo)) && caretGeo) {
            ComPtr<ID2D1GeometrySink> sink;
            if (SUCCEEDED(caretGeo->Open(&sink)) && sink) {
                sink->BeginFigure({tipX - 7.5f, cardRc.bottom - 0.5f}, D2D1_FIGURE_BEGIN_FILLED);
                sink->AddLine({tipX, cardRc.bottom + 6.5f});
                sink->AddLine({tipX + 7.5f, cardRc.bottom - 0.5f});
                sink->EndFigure(D2D1_FIGURE_END_CLOSED);
                sink->Close();
                setBrush(shellBot);
                rt->FillGeometry(caretGeo.Get(), brush);
            }
        }
    }

    // 外边缘微晶线 + 顶部镜面高光丝边
    setBrush(dark ? rgba(1.0f, 1.0f, 1.0f, 0.14f) : rgba(0.06f, 0.10f, 0.18f, 0.11f));
    rt->DrawRoundedRectangle(D2D1::RoundedRect(cardRc, 14.5f, 14.5f), brush, 1.0f);
    if (!dark) {
        setBrush(rgba(1.0f, 1.0f, 1.0f, 0.92f));
        rt->DrawRoundedRectangle(
            D2D1::RoundedRect(D2D1::RectF(cardRc.left + 1.0f, cardRc.top + 1.0f, cardRc.right - 1.0f, cardRc.bottom - 1.0f), 13.5f, 13.5f),
            brush, 1.0f);
    }

    // 3. 上舱：Apple / Figma 级深浅自适应微缩剧场画布 (312 x 112)
    const D2D1_RECT_F stageRc = D2D1::RectF(cardRc.left + 11.0f, cardRc.top + 11.0f, cardRc.right - 11.0f, cardRc.top + 123.0f);
    const D2D1_COLOR_F stTop = dark ? rgba(0.048f, 0.068f, 0.118f, 1.0f) : rgba(0.933f, 0.953f, 0.980f, 1.0f);
    const D2D1_COLOR_F stBot = dark ? rgba(0.075f, 0.105f, 0.176f, 1.0f) : rgba(0.890f, 0.922f, 0.965f, 1.0f);
    fillVGradRounded(stageRc, 10.5f, stTop, stBot);

    const float stL = stageRc.left;
    const float stT = stageRc.top;
    const float stW = stageRc.right - stageRc.left;
    const float stH = stageRc.bottom - stageRc.top;
    const float stCx = stL + stW * 0.5f;
    const float stCy = stT + stH * 0.5f;

    rt->PushAxisAlignedClip(stageRc, D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);

    // 柔和环境氛围光晕球 + 细腻亚像素微点阵 (Dot Matrix)
    fillRadialGlow(stCx, stCy + 6.0f, stW * 0.46f, stH * 0.58f,
                   rgba(accentR, accentG, accentB, dark ? 0.16f : 0.11f));
    setBrush(dark ? rgba(1.0f, 1.0f, 1.0f, 0.065f) : rgba(0.15f, 0.23f, 0.38f, 0.075f));
    for (int gy = 1; gy <= 4; ++gy) {
        const float py = stT + gy * (stH / 5.0f);
        for (int gx = 1; gx <= 9; ++gx) {
            const float px = stL + gx * (stW / 10.0f);
            rt->FillEllipse(D2D1::Ellipse({px, py}, 0.85f, 0.85f), brush);
        }
    }

    // ── 微缩剧场高精公共原语组件 ──
    auto drawRulerStrip = [&](float topY) {
        setBrush(dark ? rgba(1.0f, 1.0f, 1.0f, 0.05f) : rgba(0.08f, 0.14f, 0.25f, 0.045f));
        rt->FillRectangle(D2D1::RectF(stL, topY, stageRc.right, topY + 15.0f), brush);
        setBrush(dark ? rgba(1.0f, 1.0f, 1.0f, 0.18f) : rgba(0.20f, 0.28f, 0.42f, 0.22f));
        for (int t = 0; t <= 16; ++t) {
            const float tx = stL + 16.0f + t * ((stW - 32.0f) / 16.0f);
            const float th = (t % 4 == 0) ? 5.5f : 3.0f;
            rt->DrawLine({tx, topY + 15.0f - th}, {tx, topY + 15.0f}, brush, 1.0f);
        }
    };

    // 拟真胶片轨道块（丝绸渐变 + 顶部镜面高光 + 微缩预览窗 + 圆润声波谱）
    auto drawCinemaClip = [&](const D2D1_RECT_F& rc, const D2D1_COLOR_F& c0, const D2D1_COLOR_F& c1,
                              int waveSeed, float alphaMul = 1.0f, bool selectedGlow = false) {
        const float w = rc.right - rc.left;
        const float h = rc.bottom - rc.top;
        if (w < 8.0f || h < 8.0f || alphaMul <= 0.02f) return;

        // 底部柔和投影
        setBrush(rgba(0.02f, 0.05f, 0.12f, (dark ? 0.34f : 0.14f) * alphaMul));
        rt->FillRoundedRectangle(
            D2D1::RoundedRect(D2D1::RectF(rc.left, rc.top + 2.2f, rc.right, rc.bottom + 2.6f), 7.0f, 7.0f), brush);

        fillVGradRounded(rc, 6.8f,
                         D2D1::ColorF(c0.r, c0.g, c0.b, c0.a * alphaMul),
                         D2D1::ColorF(c1.r, c1.g, c1.b, c1.a * alphaMul));

        // 顶部 1px 玻璃高光丝边
        setBrush(rgba(1.0f, 1.0f, 1.0f, (selectedGlow ? 0.90f : 0.36f) * alphaMul));
        rt->DrawRoundedRectangle(
            D2D1::RoundedRect(D2D1::RectF(rc.left + 0.6f, rc.top + 0.6f, rc.right - 0.6f, rc.bottom - 0.6f), 6.2f, 6.2f),
            brush, selectedGlow ? 1.6f : 0.9f);

        float waveStartL = rc.left + 8.0f;
        // 若宽度充裕，左侧绘制一枚精致的微缩画面帧缩略窗
        if (w >= 54.0f) {
            const D2D1_RECT_F thumb = D2D1::RectF(rc.left + 5.0f, rc.top + 5.0f, rc.left + 27.0f, rc.bottom - 5.0f);
            setBrush(rgba(1.0f, 1.0f, 1.0f, 0.20f * alphaMul));
            rt->FillRoundedRectangle(D2D1::RoundedRect(thumb, 3.8f, 3.8f), brush);
            setBrush(rgba(1.0f, 1.0f, 1.0f, 0.72f * alphaMul));
            rt->DrawLine({thumb.left + 4.0f, thumb.top + 6.0f}, {thumb.right - 5.0f, thumb.top + 6.0f}, brush, 1.8f, roundStroke.Get());
            setBrush(rgba(1.0f, 1.0f, 1.0f, 0.45f * alphaMul));
            rt->DrawLine({thumb.left + 4.0f, thumb.top + 11.0f}, {thumb.right - 9.0f, thumb.top + 11.0f}, brush, 1.5f, roundStroke.Get());
            waveStartL = thumb.right + 7.0f;
        }

        // 圆头律动声波音柱
        const float midY = (rc.top + rc.bottom) * 0.5f;
        setBrush(rgba(1.0f, 1.0f, 1.0f, 0.52f * alphaMul));
        int idx = 0;
        for (float wx = waveStartL; wx <= rc.right - 7.0f; wx += 5.2f, ++idx) {
            const float amp = 2.4f + static_cast<float>(((idx + waveSeed) * 11 + 5) % 8) * 0.88f;
            rt->DrawLine({wx, midY - amp}, {wx, midY + amp}, brush, 1.7f, roundStroke.Get());
        }
    };

    // 拟真 Apple Magic 立体键帽（按下时物理下沉并点亮柔和背光）
    auto drawKeycap3D = [&](float rightX, float topY, const std::wstring& label, float press01) {
        float estW = 0.0f;
        for (wchar_t wc : label) estW += (wc > 0x7F) ? 12.0f : 7.0f;
        const float kw = (std::max)(26.0f, estW + 13.0f);
        const float kh = 21.0f;
        const float p = std::clamp(press01, 0.0f, 1.0f);
        const float sinkY = p * 1.8f;
        const D2D1_RECT_F baseRc = D2D1::RectF(rightX - kw, topY + 2.2f, rightX, topY + kh + 2.2f);
        const D2D1_RECT_F faceRc = D2D1::RectF(rightX - kw, topY + sinkY, rightX, topY + kh + sinkY);

        if (p > 0.05f) {
            fillRadialGlow((faceRc.left + faceRc.right) * 0.5f, (faceRc.top + faceRc.bottom) * 0.5f,
                           kw * 0.85f, kh * 0.95f, rgba(accentR, accentG, accentB, 0.36f * p));
        }

        // 立体键托厚度层
        setBrush(dark ? rgba(0.02f, 0.03f, 0.06f, 0.85f) : rgba(0.56f, 0.63f, 0.74f, 0.65f));
        rt->FillRoundedRectangle(D2D1::RoundedRect(baseRc, 5.2f, 5.2f), brush);

        // 键帽顶面丝绸渐变
        const D2D1_COLOR_F fTop = (p > 0.25f)
            ? rgba(accentR, accentG, accentB, 0.96f)
            : (dark ? rgba(0.20f, 0.25f, 0.36f, 0.98f) : rgba(1.0f, 1.0f, 1.0f, 0.99f));
        const D2D1_COLOR_F fBot = (p > 0.25f)
            ? rgba(accentR * 0.82f, accentG * 0.82f, accentB * 0.88f, 0.96f)
            : (dark ? rgba(0.14f, 0.18f, 0.27f, 0.98f) : rgba(0.93f, 0.95f, 0.98f, 0.99f));
        fillVGradRounded(faceRc, 5.0f, fTop, fBot);

        setBrush(p > 0.25f ? rgba(1.0f, 1.0f, 1.0f, 0.55f)
                           : (dark ? rgba(1.0f, 1.0f, 1.0f, 0.22f) : rgba(0.10f, 0.16f, 0.28f, 0.16f)));
        rt->DrawRoundedRectangle(D2D1::RoundedRect(faceRc, 5.0f, 5.0f), brush, 0.9f);

        const D2D1_COLOR_F txtCol = (p > 0.25f)
            ? rgba(1.0f, 1.0f, 1.0f, 1.0f)
            : (dark ? rgba(0.94f, 0.96f, 1.0f, 0.96f) : rgba(0.12f, 0.18f, 0.29f, 0.95f));
        drawTextClip(label, fonts.monoFmt, faceRc, txtCol, DWRITE_TEXT_ALIGNMENT_CENTER);
    };

    // 钻石冠播放头指针
    auto drawPlayheadPin = [&](float px, float topY, float botY, float glow01) {
        if (glow01 > 0.02f) {
            fillRadialGlow(px, (topY + botY) * 0.5f, 22.0f, (botY - topY) * 0.55f,
                           rgba(0.99f, 0.56f, 0.12f, 0.34f * glow01));
        }
        setBrush(rgba(0.98f, 0.52f, 0.08f, 0.98f));
        rt->DrawLine({px, topY + 7.0f}, {px, botY}, brush, 2.0f, roundStroke.Get());
        ComPtr<ID2D1PathGeometry> headGeo;
        if (SUCCEEDED(d2dFactory->CreatePathGeometry(&headGeo)) && headGeo) {
            ComPtr<ID2D1GeometrySink> sk;
            if (SUCCEEDED(headGeo->Open(&sk)) && sk) {
                sk->BeginFigure({px - 5.0f, topY}, D2D1_FIGURE_BEGIN_FILLED);
                sk->AddLine({px + 5.0f, topY});
                sk->AddLine({px + 5.0f, topY + 5.0f});
                sk->AddLine({px, topY + 9.5f});
                sk->AddLine({px - 5.0f, topY + 5.0f});
                sk->EndFigure(D2D1_FIGURE_END_CLOSED);
                sk->Close();
                rt->FillGeometry(headGeo.Get(), brush);
                setBrush(rgba(1.0f, 1.0f, 1.0f, 0.85f));
                rt->DrawGeometry(headGeo.Get(), brush, 0.9f, roundStroke.Get());
            }
        }
    };

    // 高精微缩苹果风矢量鼠标指针 + 径向点击柔光波
    auto drawMicroCursor = [&](float tipXPos, float tipYPos, float clickPress01, float ripple01) {
        if (ripple01 > 0.01f && ripple01 < 0.99f) {
            const float rr = 5.0f + ripple01 * 18.0f;
            setBrush(rgba(accentR, accentG, accentB, (1.0f - ripple01) * 0.68f));
            rt->DrawEllipse(D2D1::Ellipse({tipXPos, tipYPos}, rr, rr), brush, 1.8f * (1.0f - ripple01 * 0.5f));
        }
        const float sc = 1.0f - 0.16f * std::clamp(clickPress01, 0.0f, 1.0f);
        auto cp = [&](float dx, float dy) -> D2D1_POINT_2F {
            return {tipXPos + dx * sc, tipYPos + dy * sc};
        };
        ComPtr<ID2D1PathGeometry> curGeo;
        if (SUCCEEDED(d2dFactory->CreatePathGeometry(&curGeo)) && curGeo) {
            ComPtr<ID2D1GeometrySink> sk;
            if (SUCCEEDED(curGeo->Open(&sk)) && sk) {
                sk->BeginFigure(cp(0.0f, 0.0f), D2D1_FIGURE_BEGIN_FILLED);
                sk->AddLine(cp(0.0f, 15.2f));
                sk->AddLine(cp(3.8f, 11.8f));
                sk->AddLine(cp(6.6f, 17.2f));
                sk->AddLine(cp(9.0f, 16.0f));
                sk->AddLine(cp(6.2f, 10.6f));
                sk->AddLine(cp(11.0f, 10.2f));
                sk->EndFigure(D2D1_FIGURE_END_CLOSED);
                sk->Close();

                // 光标投影 + 炭黑内芯 + 纯白高反差描边
                D2D1::Matrix3x2F oldT;
                rt->GetTransform(&oldT);
                rt->SetTransform(D2D1::Matrix3x2F::Translation(1.2f, 1.8f) * oldT);
                setBrush(rgba(0.02f, 0.04f, 0.09f, 0.35f));
                rt->FillGeometry(curGeo.Get(), brush);
                rt->SetTransform(oldT);

                setBrush(rgba(0.10f, 0.13f, 0.20f, 0.98f));
                rt->FillGeometry(curGeo.Get(), brush);
                setBrush(rgba(1.0f, 1.0f, 1.0f, 0.98f));
                rt->DrawGeometry(curGeo.Get(), brush, 1.4f, roundStroke.Get());
            }
        }
    };

    // ── 7 幕高定交互动画分镜编排 ──
    if (coachHitId == Hit_SplitAtPlayhead) {
        // [幕 1: 拆分片段] 标尺 + 钻石指针滑至黄金分割点 -> [S] 立体键帽点亮 -> 光刃闪切 + 二阶弹簧平滑分裂为两枚胶片卡
        drawRulerStrip(stT);
        const float moveT = easeOutCubic(phase / 0.28f);
        const float keyPress = (phase >= 0.28f && phase <= 0.48f)
            ? std::sin(((phase - 0.28f) / 0.20f) * 3.1415926f) : 0.0f;
        const float splitT = (phase < 0.34f) ? 0.0f
            : ((phase < 0.80f) ? spring01((phase - 0.34f) / 0.32f)
                               : (1.0f - smooth01((phase - 0.80f) / 0.20f)));

        const float cutX = stCx - 4.0f;
        const float gapHalf = std::clamp(splitT, 0.0f, 1.15f) * 7.5f;
        const float barTop = stCy - 4.0f;
        const float barBot = stCy + 30.0f;
        const float barL = stL + 20.0f;
        const float barR = stageRc.right - 20.0f;

        if (gapHalf < 0.8f) {
            drawCinemaClip(D2D1::RectF(barL, barTop, barR, barBot),
                           rgba(0.24f, 0.47f, 0.96f, 0.96f), rgba(0.16f, 0.35f, 0.84f, 0.96f), 1, 1.0f, false);
        } else {
            drawCinemaClip(D2D1::RectF(barL, barTop, cutX - gapHalf, barBot),
                           rgba(0.24f, 0.47f, 0.96f, 0.96f), rgba(0.16f, 0.35f, 0.84f, 0.96f), 1, 1.0f, false);
            drawCinemaClip(D2D1::RectF(cutX + gapHalf, barTop, barR, barBot),
                           rgba(0.08f, 0.68f, 0.86f, 0.96f), rgba(0.06f, 0.50f, 0.74f, 0.96f), 6, 1.0f, true);
        }

        const float phX = (barL + 24.0f) + (cutX - (barL + 24.0f)) * moveT;
        const float cutFlash = (phase >= 0.32f && phase <= 0.54f)
            ? (1.0f - (phase - 0.32f) / 0.22f) : 0.0f;
        drawPlayheadPin(phX, stT + 11.0f, stageRc.bottom - 10.0f, cutFlash);

        if (cutFlash > 0.01f) {
            fillRadialGlow(cutX, (barTop + barBot) * 0.5f, 24.0f * (1.2f - cutFlash * 0.4f), 26.0f,
                           rgba(0.62f, 0.44f, 1.0f, 0.52f * cutFlash));
        }
        drawKeycap3D(stageRc.right - 12.0f, stT + 20.0f, L"S", keyPress);

    } else if (coachHitId == Hit_DeleteSelectedItem) {
        // [幕 2: 磁性波纹删除] 光标移入点选中间玫瑰红废片 -> [Del] 键帽按下 -> 废片化雾上浮消散 -> 右侧片段丝滑弹簧磁吸闭合
        drawRulerStrip(stT);
        const float curMove = easeOutCubic(phase / 0.22f);
        const float clickP = (phase >= 0.20f && phase <= 0.34f)
            ? std::sin(((phase - 0.20f) / 0.14f) * 3.1415926f) : 0.0f;
        const float keyP = (phase >= 0.30f && phase <= 0.48f)
            ? std::sin(((phase - 0.30f) / 0.18f) * 3.1415926f) : 0.0f;
        const float vanishT = (phase < 0.34f) ? 0.0f
            : ((phase < 0.54f) ? easeOutCubic((phase - 0.34f) / 0.20f)
                               : (phase > 0.86f ? (1.0f - smooth01((phase - 0.86f) / 0.14f)) : 1.0f));
        const float snapT = (phase < 0.48f) ? 0.0f
            : ((phase < 0.78f) ? spring01((phase - 0.48f) / 0.30f)
                               : (phase > 0.86f ? (1.0f - smooth01((phase - 0.86f) / 0.14f)) : 1.0f));

        const float y0 = stCy - 2.0f, y1 = stCy + 32.0f;
        const float c1L = stL + 18.0f, c1R = stL + 100.0f;
        const float midW = 70.0f;
        const float c2L = c1R + 4.0f, c2R = c2L + midW;
        const float shiftLeft = std::clamp(snapT, 0.0f, 1.06f) * (midW + 4.0f);
        const float c3L = (std::max)(c1R + 2.5f, c2R + 4.0f - shiftLeft);
        const float c3R = c3L + 92.0f;

        drawCinemaClip(D2D1::RectF(c1L, y0, c1R, y1),
                       rgba(0.24f, 0.47f, 0.96f, 0.96f), rgba(0.16f, 0.35f, 0.84f, 0.96f), 1, 1.0f, false);
        drawCinemaClip(D2D1::RectF(c3L, y0, c3R, y1),
                       rgba(0.10f, 0.66f, 0.86f, 0.96f), rgba(0.07f, 0.48f, 0.74f, 0.96f), 5, 1.0f, snapT > 0.6f);

        const float midAlpha = 1.0f - vanishT;
        if (midAlpha > 0.02f) {
            const float lift = vanishT * 12.0f;
            const float sx = vanishT * 10.0f;
            drawCinemaClip(D2D1::RectF(c2L + sx, y0 - lift, c2R - sx, y1 - lift),
                           rgba(0.95f, 0.33f, 0.42f, 0.94f), rgba(0.82f, 0.20f, 0.32f, 0.94f),
                           3, midAlpha, phase >= 0.18f);
        }

        // 无缝贴合瞬间翡翠绿磁吸光晕
        if (phase >= 0.64f && phase <= 0.85f) {
            const float seamGlow = std::sin(((phase - 0.64f) / 0.21f) * 3.1415926f);
            fillRadialGlow(c1R + 1.5f, (y0 + y1) * 0.5f, 18.0f, 24.0f, rgba(0.10f, 0.82f, 0.54f, 0.55f * seamGlow));
            setBrush(rgba(0.12f, 0.86f, 0.56f, 0.90f * seamGlow));
            rt->DrawLine({c1R + 1.2f, y0 - 4.0f}, {c1R + 1.2f, y1 + 4.0f}, brush, 2.0f, roundStroke.Get());
        }

        drawKeycap3D(stageRc.right - 12.0f, stT + 20.0f, L"Del", keyP);
        if (phase < 0.46f) {
            const float cxPos = (c1L + 30.0f) + ((c2L + c2R) * 0.5f - (c1L + 30.0f)) * curMove;
            const float cyPos = (y1 + 10.0f) + ((y0 + y1) * 0.5f - (y1 + 10.0f)) * curMove;
            const float rip = (phase >= 0.21f && phase <= 0.40f) ? ((phase - 0.21f) / 0.19f) : 0.0f;
            drawMicroCursor(cxPos, cyPos, clickP, rip);
        }

    } else if (coachHitId == Hit_CycleClipSpeed) {
        // [幕 3: 片段倍速] 顶部悬浮三档精致分段胶囊 [1.0x | 1.5x | 2.0x] -> 光标轻点 2.0x -> 胶片弹性浓缩加速 + 流光双箭簇掠过
        const float segW = 144.0f;
        const D2D1_RECT_F segBar = D2D1::RectF(stCx - segW * 0.5f, stT + 12.0f, stCx + segW * 0.5f, stT + 34.0f);
        setBrush(dark ? rgba(1.0f, 1.0f, 1.0f, 0.08f) : rgba(0.10f, 0.16f, 0.28f, 0.07f));
        rt->FillRoundedRectangle(D2D1::RoundedRect(segBar, 6.5f, 6.5f), brush);

        const float shiftFast = (phase < 0.26f) ? 0.0f
            : ((phase < 0.50f) ? spring01((phase - 0.26f) / 0.24f)
                               : (phase > 0.86f ? (1.0f - smooth01((phase - 0.86f) / 0.14f)) : 1.0f));
        const float cellW = (segW - 4.0f) / 3.0f;
        const float pillX = segBar.left + 2.0f + std::clamp(shiftFast, 0.0f, 1.0f) * (cellW * 2.0f);
        fillVGradRounded(D2D1::RectF(pillX, segBar.top + 2.0f, pillX + cellW, segBar.bottom - 2.0f), 5.0f,
                         rgba(0.08f, 0.74f, 0.66f, 0.96f), rgba(0.05f, 0.56f, 0.52f, 0.96f));

        static const wchar_t* spds[3] = { L"1.0x", L"1.5x", L"2.0x" };
        for (int k = 0; k < 3; ++k) {
            const D2D1_RECT_F cr = D2D1::RectF(segBar.left + 2.0f + k * cellW, segBar.top, segBar.left + 2.0f + (k + 1) * cellW, segBar.bottom);
            const bool active = (k == 0 && shiftFast < 0.4f) || (k == 2 && shiftFast >= 0.4f);
            drawTextClip(spds[k], fonts.monoFmt, cr,
                         active ? rgba(1.0f, 1.0f, 1.0f, 1.0f)
                                : (dark ? rgba(0.75f, 0.82f, 0.92f, 0.78f) : rgba(0.24f, 0.31f, 0.44f, 0.82f)),
                         DWRITE_TEXT_ALIGNMENT_CENTER);
        }

        const float barL = stL + 28.0f;
        const float barW = 252.0f - std::clamp(shiftFast, 0.0f, 1.05f) * 106.0f;
        const D2D1_RECT_F clipRc = D2D1::RectF(barL, stCy + 4.0f, barL + barW, stCy + 38.0f);
        const bool isFast = (shiftFast > 0.35f);
        drawCinemaClip(clipRc,
                       isFast ? rgba(0.08f, 0.74f, 0.66f, 0.96f) : rgba(0.24f, 0.47f, 0.96f, 0.96f),
                       isFast ? rgba(0.05f, 0.55f, 0.52f, 0.96f) : rgba(0.16f, 0.35f, 0.84f, 0.96f),
                       2, 1.0f, isFast);

        // 快进双流光箭簇 >>（带微晶胶囊底衬，与下方声波清晰分离）
        if (isFast) {
            const float sweep = std::fmod(phase * 2.6f, 1.0f);
            const float visAlpha = std::sin(sweep * 3.1415926f);
            const float cxArrow = clipRc.left + 40.0f + (barW - 60.0f) * sweep;
            const float cyArrow = (clipRc.top + clipRc.bottom) * 0.5f;
            setBrush(rgba(0.03f, 0.24f, 0.24f, 0.62f * visAlpha));
            rt->FillRoundedRectangle(
                D2D1::RoundedRect(D2D1::RectF(cxArrow - 8.5f, cyArrow - 8.0f, cxArrow + 12.5f, cyArrow + 8.0f), 5.0f, 5.0f),
                brush);
            setBrush(rgba(1.0f, 1.0f, 1.0f, 0.96f * visAlpha));
            for (int a = 0; a < 2; ++a) {
                const float ox = cxArrow - 1.5f + a * 6.8f;
                rt->DrawLine({ox - 3.2f, cyArrow - 4.0f}, {ox + 1.0f, cyArrow}, brush, 2.0f, roundStroke.Get());
                rt->DrawLine({ox - 3.2f, cyArrow + 4.0f}, {ox + 1.0f, cyArrow}, brush, 2.0f, roundStroke.Get());
            }
        }

        const float curT = easeOutCubic(phase / 0.24f);
        if (phase < 0.48f) {
            const float tx = (segBar.left + cellW * 0.5f) + (cellW * 2.0f) * curT;
            const float ty = segBar.bottom - 5.0f;
            const float cp = (phase >= 0.22f && phase <= 0.34f) ? std::sin(((phase - 0.22f) / 0.12f) * 3.1415926f) : 0.0f;
            const float rp = (phase >= 0.24f && phase <= 0.42f) ? ((phase - 0.24f) / 0.18f) : 0.0f;
            drawMicroCursor(tx, ty, cp, rp);
        }

    } else if (coachHitId == Hit_UndoBtn || coachHitId == Hit_RedoBtn) {
        // [幕 4 & 5: 撤销 Ctrl+Z / 重做 Ctrl+Y] 真正平滑贝塞尔圆弧丝带箭头 + 立体双键帽联动 + 胶片时光倒带/正向重演
        const bool isUndo = (coachHitId == Hit_UndoBtn);
        const float keyP = (phase >= 0.14f && phase <= 0.36f)
            ? std::sin(((phase - 0.14f) / 0.22f) * 3.1415926f) : 0.0f;
        const float actT = (phase < 0.20f) ? 0.0f
            : ((phase < 0.66f) ? spring01((phase - 0.20f) / 0.42f)
                               : (1.0f - smooth01((phase - 0.66f) / 0.34f)));
        const float clampedAct = std::clamp(actT, 0.0f, 1.0f);
        const float splitSep = (isUndo ? (1.0f - clampedAct) : clampedAct) * 11.0f;

        // 上方左侧：Apple SF Symbols 级镜像对称 U 型回转箭头（撤销明确朝左 <--，重做明确朝右 -->）
        const float arcCx = stCx - 48.0f;
        const float arcCy = stT + 29.0f;
        fillRadialGlow(arcCx, arcCy, 28.0f, 22.0f,
                       isUndo ? rgba(0.24f, 0.56f, 0.98f, 0.26f) : rgba(0.08f, 0.76f, 0.58f, 0.26f));
        {
            const float dir = isUndo ? -1.0f : 1.0f; // -1: 朝左回退 (Undo), +1: 朝右重演 (Redo)
            const float shiftX = dir * std::sin(clampedAct * 3.1415926f) * 3.5f;
            const float cxU = arcCx + shiftX;
            const float rU = 7.5f;
            const float topLineY = arcCy - rU;
            const float botLineY = arcCy + rU;

            ComPtr<ID2D1PathGeometry> uGeo;
            if (SUCCEEDED(d2dFactory->CreatePathGeometry(&uGeo)) && uGeo) {
                ComPtr<ID2D1GeometrySink> sk;
                if (SUCCEEDED(uGeo->Open(&sk)) && sk) {
                    // 下支脚 -> 180° 圆润外凸半圆弯折 -> 上主干直指目标方向 (Undo: ↩ 朝左, Redo: ↪ 朝右)
                    const float bendX = cxU - dir * 9.0f;
                    sk->BeginFigure({cxU + dir * 2.0f, botLineY}, D2D1_FIGURE_BEGIN_HOLLOW);
                    sk->AddLine({bendX, botLineY});
                    for (int s = 1; s <= 20; ++s) {
                        const float ang = 1.5707963f + dir * (static_cast<float>(s) / 20.0f) * 3.1415926f;
                        sk->AddLine({bendX + std::cos(ang) * rU, arcCy + std::sin(ang) * rU});
                    }
                    const D2D1_POINT_2F tipPt{cxU + dir * 11.5f, topLineY};
                    sk->AddLine(tipPt);
                    sk->EndFigure(D2D1_FIGURE_END_OPEN);
                    sk->Close();

                    const D2D1_COLOR_F arcCol = isUndo
                        ? rgba(0.24f, 0.56f, 0.98f, 0.98f)
                        : rgba(0.08f, 0.74f, 0.56f, 0.98f);
                    setBrush(arcCol);
                    rt->DrawGeometry(uGeo.Get(), brush, 2.7f, roundStroke.Get());
                    rt->DrawLine(tipPt, {tipPt.x - dir * 5.8f, tipPt.y - 4.8f}, brush, 2.6f, roundStroke.Get());
                    rt->DrawLine(tipPt, {tipPt.x - dir * 5.8f, tipPt.y + 4.8f}, brush, 2.6f, roundStroke.Get());
                }
            }
        }

        // 上方右侧：立体 [Ctrl] + [Z / Y] 组合键帽同步清脆按下
        drawKeycap3D(stCx + 42.0f, stT + 18.0f, L"Ctrl", keyP);
        drawKeycap3D(stCx + 76.0f, stT + 18.0f, isUndo ? L"Z" : L"Y", keyP);

        // 下方胶片切片平滑合拢复原 / 正向重演
        const float by0 = stT + 60.0f, by1 = stT + 92.0f;
        if (splitSep < 1.0f) {
            drawCinemaClip(D2D1::RectF(stL + 26.0f, by0, stageRc.right - 26.0f, by1),
                           rgba(0.24f, 0.47f, 0.96f, 0.96f), rgba(0.16f, 0.35f, 0.84f, 0.96f), 2, 1.0f, true);
        } else {
            drawCinemaClip(D2D1::RectF(stL + 26.0f, by0, stCx - splitSep, by1),
                           rgba(0.24f, 0.47f, 0.96f, 0.96f), rgba(0.16f, 0.35f, 0.84f, 0.96f), 2, 1.0f, false);
            drawCinemaClip(D2D1::RectF(stCx + splitSep, by0, stageRc.right - 26.0f, by1),
                           rgba(0.10f, 0.66f, 0.86f, 0.96f), rgba(0.07f, 0.48f, 0.74f, 0.96f), 7, 1.0f, true);
        }

    } else if (coachHitId == Hit_QuickOpenAnnotations) {
        // [幕 6: 画面重点标注] 精致微缩桌面窗口 + 柔顺橙色马克笔一笔画出重点圈 + 紫罗兰圆角气泡标签优雅弹出
        const D2D1_RECT_F appWin = D2D1::RectF(stL + 36.0f, stT + 10.0f, stageRc.right - 36.0f, stT + 82.0f);
        setBrush(rgba(0.03f, 0.06f, 0.14f, dark ? 0.36f : 0.12f));
        rt->FillRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(appWin.left, appWin.top + 2.5f, appWin.right, appWin.bottom + 3.0f), 8.0f, 8.0f), brush);

        fillVGradRounded(appWin, 7.5f,
                         dark ? rgba(0.13f, 0.17f, 0.27f, 0.98f) : rgba(1.0f, 1.0f, 1.0f, 0.98f),
                         dark ? rgba(0.10f, 0.13f, 0.22f, 0.98f) : rgba(0.96f, 0.98f, 1.0f, 0.98f));
        setBrush(dark ? rgba(1.0f, 1.0f, 1.0f, 0.12f) : rgba(0.10f, 0.16f, 0.28f, 0.10f));
        rt->DrawRoundedRectangle(D2D1::RoundedRect(appWin, 7.5f, 7.5f), brush, 1.0f);

        // 窗口顶部标题栏三圆点
        const float dotCols[3][3] = { {0.96f, 0.37f, 0.36f}, {0.98f, 0.74f, 0.18f}, {0.20f, 0.78f, 0.42f} };
        for (int d = 0; d < 3; ++d) {
            setBrush(rgba(dotCols[d][0], dotCols[d][1], dotCols[d][2], 0.90f));
            rt->FillEllipse(D2D1::Ellipse({appWin.left + 11.0f + d * 8.0f, appWin.top + 9.0f}, 2.4f, 2.4f), brush);
        }
        // 窗口内模拟 UI 元素与重点按钮
        setBrush(dark ? rgba(1.0f, 1.0f, 1.0f, 0.20f) : rgba(0.20f, 0.28f, 0.42f, 0.18f));
        rt->DrawLine({appWin.left + 14.0f, appWin.top + 26.0f}, {appWin.left + 102.0f, appWin.top + 26.0f}, brush, 3.2f, roundStroke.Get());
        rt->DrawLine({appWin.left + 14.0f, appWin.top + 62.0f}, {appWin.left + 84.0f, appWin.top + 62.0f}, brush, 3.0f, roundStroke.Get());

        const D2D1_RECT_F targetBtn = D2D1::RectF(appWin.left + 14.0f, appWin.top + 36.0f, appWin.left + 88.0f, appWin.top + 52.0f);
        fillVGradRounded(targetBtn, 4.5f, rgba(0.22f, 0.52f, 0.96f, 0.25f), rgba(0.22f, 0.52f, 0.96f, 0.16f));
        setBrush(rgba(0.24f, 0.56f, 0.98f, 0.85f));
        rt->DrawLine({targetBtn.left + 8.0f, (targetBtn.top + targetBtn.bottom) * 0.5f},
                     {targetBtn.right - 8.0f, (targetBtn.top + targetBtn.bottom) * 0.5f}, brush, 3.0f, roundStroke.Get());

        // 一笔丝滑画出手绘重点圈 (Marker Loop)
        const float drawLoopT = (phase < 0.12f) ? 0.0f
            : ((phase < 0.48f) ? easeOutCubic((phase - 0.12f) / 0.36f)
                               : (phase > 0.86f ? (1.0f - smooth01((phase - 0.86f) / 0.14f)) : 1.0f));
        if (drawLoopT > 0.02f) {
            const float lcx = (targetBtn.left + targetBtn.right) * 0.5f;
            const float lcy = (targetBtn.top + targetBtn.bottom) * 0.5f;
            ComPtr<ID2D1PathGeometry> loopGeo;
            if (SUCCEEDED(d2dFactory->CreatePathGeometry(&loopGeo)) && loopGeo) {
                ComPtr<ID2D1GeometrySink> sk;
                if (SUCCEEDED(loopGeo->Open(&sk)) && sk) {
                    constexpr int kN = 64;
                    const int endN = (std::max)(3, static_cast<int>(std::round(kN * drawLoopT)));
                    auto ptLoop = [&](int i) -> D2D1_POINT_2F {
                        const float u = static_cast<float>(i) / kN;
                        const float th = -1.88f + u * 6.84f;
                        const float sp = 0.95f + 0.10f * u;
                        return {lcx + std::cos(th) * 44.0f * sp, lcy + std::sin(th) * 12.5f * sp};
                    };
                    sk->BeginFigure(ptLoop(0), D2D1_FIGURE_BEGIN_HOLLOW);
                    for (int i = 1; i <= endN; ++i) sk->AddLine(ptLoop(i));
                    sk->EndFigure(D2D1_FIGURE_END_OPEN);
                    sk->Close();
                    setBrush(rgba(0.98f, 0.45f, 0.09f, 0.24f));
                    rt->DrawGeometry(loopGeo.Get(), brush, 5.2f, roundStroke.Get());
                    setBrush(rgba(0.98f, 0.45f, 0.09f, 0.98f));
                    rt->DrawGeometry(loopGeo.Get(), brush, 2.4f, roundStroke.Get());
                }
            }
        }

        // 紫罗兰圆角对话气泡优雅弹出
        const float popT = (phase < 0.34f) ? 0.0f
            : ((phase < 0.62f) ? spring01((phase - 0.34f) / 0.28f)
                               : (phase > 0.86f ? (1.0f - smooth01((phase - 0.86f) / 0.14f)) : 1.0f));
        if (popT > 0.04f) {
            const float sc = std::clamp(popT, 0.0f, 1.08f);
            const float bx = targetBtn.right + 42.0f;
            const float by = targetBtn.top + 4.0f - (1.0f - std::min(1.0f, sc)) * 6.0f;
            const float hw = 30.0f * sc, hh = 11.0f * sc;
            const D2D1_RECT_F bub = D2D1::RectF(bx - hw, by - hh, bx + hw, by + hh);
            fillVGradRounded(bub, 6.0f, rgba(0.66f, 0.34f, 0.98f, 0.96f * std::min(1.0f, sc)),
                             rgba(0.52f, 0.22f, 0.88f, 0.96f * std::min(1.0f, sc)));
            drawTextClip(isZh ? L"重点" : L"Key", fonts.captionFmt, bub, rgba(1.0f, 1.0f, 1.0f, std::min(1.0f, sc)),
                         DWRITE_TEXT_ALIGNMENT_CENTER);
        }

        // 底部橙金标注时间段同步点亮
        const float segW = 108.0f * std::clamp(drawLoopT, 0.15f, 1.0f);
        fillVGradRounded(D2D1::RectF(stCx - segW * 0.5f, stT + 90.0f, stCx + segW * 0.5f, stT + 103.0f), 4.2f,
                         rgba(0.98f, 0.60f, 0.10f, 0.95f), rgba(0.90f, 0.44f, 0.06f, 0.95f));

    } else if (coachHitId == Hit_AddZoomAtPlayhead) {
        // [幕 7: 插入镜头推近特写] 精致桌面窗口 + 光学四角取景框 + 丝滑弹簧推近 1.9x + 光标拖动准星平移焦点
        const float zoomInT = (phase < 0.14f) ? 0.0f
            : ((phase < 0.46f) ? spring01((phase - 0.14f) / 0.32f)
                               : ((phase < 0.82f) ? 1.0f : (1.0f - smooth01((phase - 0.82f) / 0.18f))));
        const float panT = (phase < 0.48f) ? 0.0f
            : ((phase < 0.76f) ? smooth01((phase - 0.48f) / 0.28f)
                               : (1.0f - smooth01((phase - 0.76f) / 0.24f)));

        const float scale = 1.0f + std::clamp(zoomInT, 0.0f, 1.06f) * 0.68f;
        const D2D1_RECT_F viewWin = D2D1::RectF(stL + 36.0f, stT + 8.0f, stageRc.right - 36.0f, stT + 80.0f);
        fillVGradRounded(viewWin, 7.5f,
                         dark ? rgba(0.12f, 0.16f, 0.26f, 0.98f) : rgba(1.0f, 1.0f, 1.0f, 0.98f),
                         dark ? rgba(0.09f, 0.12f, 0.20f, 0.98f) : rgba(0.95f, 0.97f, 1.0f, 0.98f));

        rt->PushAxisAlignedClip(viewWin, D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);
        const float focusX = viewWin.left + 76.0f + panT * 52.0f;
        const float focusY = viewWin.top + 36.0f - panT * 6.0f;
        auto zRect = [&](float x0, float y0, float x1, float y1) -> D2D1_RECT_F {
            return D2D1::RectF(
                focusX + (x0 - focusX) * scale, focusY + (y0 - focusY) * scale,
                focusX + (x1 - focusX) * scale, focusY + (y1 - focusY) * scale);
        };

        // 模拟界面卡片随镜头平滑推近放大
        const D2D1_RECT_F cardA = zRect(viewWin.left + 16.0f, viewWin.top + 16.0f, viewWin.left + 112.0f, viewWin.top + 58.0f);
        const D2D1_RECT_F cardB = zRect(viewWin.left + 122.0f, viewWin.top + 16.0f, viewWin.right - 16.0f, viewWin.top + 58.0f);
        fillVGradRounded(cardA, 5.0f * scale, rgba(0.24f, 0.56f, 0.98f, 0.18f), rgba(0.24f, 0.56f, 0.98f, 0.10f));
        fillVGradRounded(cardB, 5.0f * scale, rgba(0.10f, 0.76f, 0.58f, 0.18f), rgba(0.10f, 0.76f, 0.58f, 0.10f));

        setBrush(rgba(0.24f, 0.56f, 0.98f, 0.85f));
        rt->DrawLine({cardA.left + 8.0f * scale, cardA.top + 12.0f * scale},
                     {cardA.right - 14.0f * scale, cardA.top + 12.0f * scale}, brush, 2.8f * scale, roundStroke.Get());
        setBrush(dark ? rgba(1.0f, 1.0f, 1.0f, 0.30f) : rgba(0.20f, 0.28f, 0.42f, 0.28f));
        rt->DrawLine({cardA.left + 8.0f * scale, cardA.top + 23.0f * scale},
                     {cardA.right - 26.0f * scale, cardA.top + 23.0f * scale}, brush, 2.2f * scale, roundStroke.Get());

        setBrush(rgba(0.08f, 0.72f, 0.54f, 0.85f));
        rt->DrawLine({cardB.left + 8.0f * scale, cardB.top + 12.0f * scale},
                     {cardB.right - 16.0f * scale, cardB.top + 12.0f * scale}, brush, 2.8f * scale, roundStroke.Get());

        // 光学四角取景导角框 + 中心十字对焦环
        const float vfW = 34.0f, vfH = 20.0f, cLen = 6.5f;
        setBrush(rgba(accentR, accentG, accentB, 0.92f));
        const float vx0 = focusX - vfW, vy0 = focusY - vfH, vx1 = focusX + vfW, vy1 = focusY + vfH;
        rt->DrawLine({vx0, vy0 + cLen}, {vx0, vy0}, brush, 1.8f, roundStroke.Get());
        rt->DrawLine({vx0, vy0}, {vx0 + cLen, vy0}, brush, 1.8f, roundStroke.Get());
        rt->DrawLine({vx1 - cLen, vy0}, {vx1, vy0}, brush, 1.8f, roundStroke.Get());
        rt->DrawLine({vx1, vy0}, {vx1, vy0 + cLen}, brush, 1.8f, roundStroke.Get());
        rt->DrawLine({vx0, vy1 - cLen}, {vx0, vy1}, brush, 1.8f, roundStroke.Get());
        rt->DrawLine({vx0, vy1}, {vx0 + cLen, vy1}, brush, 1.8f, roundStroke.Get());
        rt->DrawLine({vx1 - cLen, vy1}, {vx1, vy1}, brush, 1.8f, roundStroke.Get());
        rt->DrawLine({vx1, vy1}, {vx1, vy1 - cLen}, brush, 1.8f, roundStroke.Get());

        rt->DrawEllipse(D2D1::Ellipse({focusX, focusY}, 6.5f, 6.5f), brush, 1.5f);
        rt->DrawLine({focusX - 10.0f, focusY}, {focusX + 10.0f, focusY}, brush, 1.2f, roundStroke.Get());
        rt->DrawLine({focusX, focusY - 10.0f}, {focusX, focusY + 10.0f}, brush, 1.2f, roundStroke.Get());

        if (phase >= 0.44f && phase <= 0.80f) {
            drawMicroCursor(focusX + 2.0f, focusY + 2.0f, 0.85f, 0.0f);
        }
        rt->PopAxisAlignedClip();

        setBrush(dark ? rgba(1.0f, 1.0f, 1.0f, 0.14f) : rgba(0.10f, 0.16f, 0.28f, 0.12f));
        rt->DrawRoundedRectangle(D2D1::RoundedRect(viewWin, 7.5f, 7.5f), brush, 1.0f);

        // 底部蓝色渐变运镜轨段同步亮起
        const D2D1_RECT_F zSeg = D2D1::RectF(stCx - 54.0f, stT + 88.0f, stCx + 54.0f, stT + 104.0f);
        fillVGradRounded(zSeg, 4.5f,
                         rgba(0.22f, 0.54f, 0.98f, 0.55f + 0.42f * std::clamp(zoomInT, 0.0f, 1.0f)),
                         rgba(0.14f, 0.38f, 0.88f, 0.55f + 0.42f * std::clamp(zoomInT, 0.0f, 1.0f)));
        drawTextClip(L"1.9x", fonts.monoFmt, zSeg, rgba(1.0f, 1.0f, 1.0f, 0.96f), DWRITE_TEXT_ALIGNMENT_CENTER);
    }

    rt->PopAxisAlignedClip();

    // 视窗精致内镶边框
    setBrush(dark ? rgba(1.0f, 1.0f, 1.0f, 0.10f) : rgba(0.08f, 0.14f, 0.26f, 0.09f));
    rt->DrawRoundedRectangle(D2D1::RoundedRect(stageRc, 10.5f, 10.5f), brush, 1.0f);

    // 4. 下舱：清晰功能主标题 + 右侧立体微晶胶囊 + 舒适行距双行说明
    const float txtL = cardRc.left + 15.0f;
    const float txtR = cardRc.right - 15.0f;
    const float hdrY = cardRc.top + 131.0f;

    const D2D1_COLOR_F colPrimary = dark ? rgba(0.96f, 0.97f, 0.99f, 1.0f) : rgba(0.06f, 0.09f, 0.16f, 1.0f);
    const D2D1_COLOR_F colSecondary = dark ? rgba(0.68f, 0.74f, 0.84f, 1.0f) : rgba(0.32f, 0.38f, 0.48f, 1.0f);
    const D2D1_COLOR_F colAccent = rgba(accentR, accentG, accentB, 1.0f);

    float estKbdW = 0.0f;
    for (wchar_t wc : shortcutTxt) {
        estKbdW += (wc > 0x7F) ? 12.8f : 7.2f;
    }
    const float kbdW = std::clamp(estKbdW + 16.0f, 36.0f, 98.0f);
    const D2D1_RECT_F kbdPill = D2D1::RectF(txtR - kbdW, hdrY + 1.0f, txtR, hdrY + 21.0f);
    setBrush(rgba(accentR, accentG, accentB, dark ? 0.20f : 0.10f));
    rt->FillRoundedRectangle(D2D1::RoundedRect(kbdPill, 5.5f, 5.5f), brush);
    setBrush(rgba(accentR, accentG, accentB, dark ? 0.42f : 0.24f));
    rt->DrawRoundedRectangle(D2D1::RoundedRect(kbdPill, 5.5f, 5.5f), brush, 0.9f);
    drawTextClip(shortcutTxt, fonts.monoFmt, kbdPill, colAccent, DWRITE_TEXT_ALIGNMENT_CENTER);

    drawTextClip(titleTxt, fonts.sectionFmt,
                 D2D1::RectF(txtL, hdrY, kbdPill.left - 8.0f, hdrY + 22.0f), colPrimary);

    drawTextClip(descTxt, fonts.captionWrapFmt,
                 D2D1::RectF(txtL, hdrY + 25.0f, txtR, cardRc.bottom - 9.0f), colSecondary);
}

} // namespace

void StudioPreviewWindow::renderCoachTooltipOverlay(
    float W, float H, float minLeft,
    bool dark, bool isZh, ID2D1SolidColorBrush* brush) {
    (void)H;
    if (m_hoveredCoachId == 0 || m_exportModalOpen) return;

    const CoachHoverZone* targetZone = nullptr;
    for (const auto& z : m_coachHoverZones) {
        if (z.id == m_hoveredCoachId) {
            targetZone = &z;
            break;
        }
    }
    if (!targetZone) return;

    const ULONGLONG now64 = GetTickCount64();
    if (now64 < m_coachHoverEnterTick) return;
    const ULONGLONG hoverElapsedMs = now64 - m_coachHoverEnterTick;
    // 首次悬停 130ms 防误扫微延迟后在 110ms 内丝滑淡入；相邻按钮横滑时已预置提前量实现 0ms 无缝瞬切
    if (hoverElapsedMs < 130) return;
    m_coachLastActiveTick = now64;

    const float fadeAlpha = std::clamp(static_cast<float>(hoverElapsedMs - 130) / 110.0f, 0.0f, 1.0f);
    // 3.0 秒丝滑无缝循环归一化时间相位 [0.0 .. 1.0)
    const float phase = static_cast<float>((now64 % 3000ULL)) / 3000.0f;

    const float cardW = 336.0f;
    const float cardH = 212.0f;
    const float anchorCx = (targetZone->rect.left + targetZone->rect.right) * 0.5f;
    const float cardL = std::clamp(anchorCx - cardW * 0.5f, minLeft + 4.0f, (std::max)(minLeft + 4.0f, W - cardW - 14.0f));
    const float cardT = (std::max)(48.0f, targetZone->rect.top - cardH - 11.0f);
    const D2D1_RECT_F cardRc = D2D1::RectF(cardL, cardT, cardL + cardW, cardT + cardH);

    const auto accentRgb = tools3000::core::NativeThemePalette::getEffectiveAccentColorRGB();
    const CoachCinemaFonts fonts{
        m_sectionFmt.Get(),
        m_captionFmt.Get(),
        m_captionWrapFmt.Get(),
        m_monoFmt.Get()
    };
    renderLuxuryCoachCardScene(
        m_d2dFactory.Get(), m_renderTarget.Get(), brush, fonts,
        m_hoveredCoachId, targetZone->enabled, cardRc, anchorCx,
        phase, fadeAlpha, dark, isZh,
        accentRgb.r, accentRgb.g, accentRgb.b);
}

cv::Mat StudioPreviewWindow::renderCoachCardPreviewForTest(int coachSlot, float phase01, bool dark, bool isZh) {
    static const int kCoachIds[7] = {
        Hit_SplitAtPlayhead, Hit_DeleteSelectedItem, Hit_CycleClipSpeed,
        Hit_UndoBtn, Hit_RedoBtn, Hit_QuickOpenAnnotations, Hit_AddZoomAtPlayhead
    };
    if (coachSlot < 0 || coachSlot >= 7) return {};

    ComPtr<ID2D1Factory> d2d;
    ComPtr<IDWriteFactory> dw;
    if (FAILED(D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, d2d.GetAddressOf())) || !d2d) return {};
    if (FAILED(DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory),
                                   reinterpret_cast<IUnknown**>(dw.GetAddressOf()))) || !dw) return {};

    const wchar_t* uiFont = tools3000::core::resolveInstalledUiFontFamily(dw.Get());
    const wchar_t* monoFont = tools3000::core::resolveInstalledMonoFontFamily(dw.Get());
    ComPtr<IDWriteTextFormat> secFmt, capFmt, wrapFmt, monoFmt;
    dw->CreateTextFormat(uiFont, nullptr, DWRITE_FONT_WEIGHT_SEMI_BOLD, DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, 12.8f, L"zh-CN", secFmt.GetAddressOf());
    dw->CreateTextFormat(uiFont, nullptr, DWRITE_FONT_WEIGHT_MEDIUM, DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, 11.9f, L"zh-CN", capFmt.GetAddressOf());
    dw->CreateTextFormat(uiFont, nullptr, DWRITE_FONT_WEIGHT_MEDIUM, DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, 11.9f, L"zh-CN", wrapFmt.GetAddressOf());
    dw->CreateTextFormat(monoFont, nullptr, DWRITE_FONT_WEIGHT_SEMI_BOLD, DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, 11.9f, L"en-US", monoFmt.GetAddressOf());
    if (wrapFmt) {
        wrapFmt->SetWordWrapping(DWRITE_WORD_WRAPPING_WRAP);
        wrapFmt->SetLineSpacing(DWRITE_LINE_SPACING_METHOD_UNIFORM, 17.2f, 13.8f);
    }

    constexpr float kScale = 1.5f; // 150% HiDPI 高清离屏渲染
    constexpr int kLogW = 368;
    constexpr int kLogH = 244;
    constexpr int kPhysW = static_cast<int>(kLogW * kScale);
    constexpr int kPhysH = static_cast<int>(kLogH * kScale);

    BITMAPINFO bmi{};
    bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bmi.bmiHeader.biWidth = kPhysW;
    bmi.bmiHeader.biHeight = -kPhysH;
    bmi.bmiHeader.biPlanes = 1;
    bmi.bmiHeader.biBitCount = 32;
    bmi.bmiHeader.biCompression = BI_RGB;

    HDC memDc = CreateCompatibleDC(nullptr);
    if (!memDc) return {};
    void* bits = nullptr;
    HBITMAP hBmp = CreateDIBSection(memDc, &bmi, DIB_RGB_COLORS, &bits, nullptr, 0);
    if (!hBmp || !bits) {
        if (hBmp) DeleteObject(hBmp);
        DeleteDC(memDc);
        return {};
    }
    HGDIOBJ oldBmp = SelectObject(memDc, hBmp);

    D2D1_RENDER_TARGET_PROPERTIES props = D2D1::RenderTargetProperties(
        D2D1_RENDER_TARGET_TYPE_DEFAULT,
        D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED),
        96.0f * kScale, 96.0f * kScale);
    ComPtr<ID2D1DCRenderTarget> rt;
    cv::Mat outBgr;
    if (SUCCEEDED(d2d->CreateDCRenderTarget(&props, rt.GetAddressOf())) && rt) {
        RECT bindRc{0, 0, kPhysW, kPhysH};
        if (SUCCEEDED(rt->BindDC(memDc, &bindRc))) {
            rt->BeginDraw();
            rt->Clear(dark ? D2D1::ColorF(0.055f, 0.070f, 0.105f, 1.0f)
                           : D2D1::ColorF(0.955f, 0.968f, 0.985f, 1.0f));
            ComPtr<ID2D1SolidColorBrush> br;
            rt->CreateSolidColorBrush(D2D1::ColorF(1, 1, 1, 1), br.GetAddressOf());
            if (br) {
                const D2D1_RECT_F cardRc = D2D1::RectF(16.0f, 14.0f, 16.0f + 336.0f, 14.0f + 212.0f);
                const CoachCinemaFonts fonts{secFmt.Get(), capFmt.Get(), wrapFmt.Get(), monoFmt.Get()};
                renderLuxuryCoachCardScene(
                    d2d.Get(), rt.Get(), br.Get(), fonts,
                    kCoachIds[coachSlot], true, cardRc, 16.0f + 168.0f,
                    std::clamp(phase01, 0.0f, 0.999f), 1.0f, dark, isZh,
                    0.15f, 0.46f, 0.96f);
            }
            rt->EndDraw();
            cv::Mat bgra(kPhysH, kPhysW, CV_8UC4, bits);
            cv::cvtColor(bgra, outBgr, cv::COLOR_BGRA2BGR);
        }
    }

    SelectObject(memDc, oldBmp);
    DeleteObject(hBmp);
    DeleteDC(memDc);
    return outBgr;
}

namespace {

void renderBroadcastVernierTimeRuler(
    ID2D1RenderTarget* rt,
    ID2D1SolidColorBrush* brush,
    IDWriteTextFormat* monoFmt,
    float laneLeft,
    float laneRight,
    float rulerTopY,
    float rulerBottomY,
    double totalKeptSec,
    double viewStartSec,
    double viewSpanSec,
    bool dark,
    const D2D1_COLOR_F& colSecondary) {
    if (!rt || !brush || laneRight <= laneLeft + 16.0f || rulerBottomY <= rulerTopY + 6.0f) {
        return;
    }

    auto setBrush = [&](const D2D1_COLOR_F& c) { brush->SetColor(c); };
    const float trackW = laneRight - laneLeft;
    const double safeTotal = (std::max)(0.25, totalKeptSec);
    const double safeSpan = std::clamp(viewSpanSec, 0.20, safeTotal);
    const double safeStart = std::clamp(viewStartSec, 0.0, (std::max)(0.0, safeTotal - safeSpan));
    const double safeEnd = safeStart + safeSpan;

    // 标尺底边精密导轨基线
    setBrush(dark ? D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.08f) : D2D1::ColorF(0.08f, 0.14f, 0.24f, 0.09f));
    rt->DrawLine({laneLeft, rulerBottomY}, {laneRight, rulerBottomY}, brush, 0.85f);

    static constexpr double kNiceSteps[] = {
        0.2, 0.5, 1.0, 2.0, 5.0, 10.0, 15.0, 30.0, 60.0, 120.0, 300.0, 600.0, 900.0, 1800.0, 3600.0, 7200.0
    };
    const int maxMajorTicks = (std::max)(3, static_cast<int>(trackW / 108.0f));
    double majorStepSec = kNiceSteps[std::size(kNiceSteps) - 1];
    for (double cand : kNiceSteps) {
        if (safeSpan / cand <= static_cast<double>(maxMajorTicks)) {
            majorStepSec = cand;
            break;
        }
    }

    const float majorPx = static_cast<float>((majorStepSec / safeSpan) * trackW);
    // 强制采用偶数细分格数，使 subDivs / 2 100% 精准落在两个主刻度正中央（半秒/半格中线）！
    int subDivs = 10;
    const bool isSexagesimal = (std::abs(majorStepSec - 15.0) < 1e-3 ||
                                std::abs(majorStepSec - 30.0) < 1e-3 ||
                                std::abs(majorStepSec - 900.0) < 1e-3 ||
                                std::abs(majorStepSec - 1800.0) < 1e-3);
    if (isSexagesimal) {
        subDivs = (majorPx >= 96.0f) ? 6 : ((majorPx >= 48.0f) ? 2 : 2);
    } else if (majorPx >= 76.0f) {
        subDivs = 10; // 0.1s 十分位短齿 + 0.5s 挺拔半格中线
    } else if (majorPx >= 42.0f) {
        subDivs = 4;  // 0.25s 四分位短齿 + 0.5s 挺拔半格中线
    } else {
        subDivs = 2;  // 至少保留正中无字半格中线
    }
    const int halfSub = subDivs / 2;
    const double subStepSec = majorStepSec / static_cast<double>(subDivs);

    const int64_t firstSi = (std::max<int64_t>)(0, static_cast<int64_t>(std::floor(safeStart / subStepSec)));
    const int64_t lastSi  = static_cast<int64_t>(std::ceil(safeEnd / subStepSec + 1e-6));
    const int64_t maxGuardTicks = firstSi + 900;

    rt->PushAxisAlignedClip(
        D2D1::RectF(laneLeft - 1.0f, rulerTopY, laneRight + 1.0f, rulerBottomY + 1.0f),
        D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);

    for (int64_t si = firstSi; si <= lastSi && si <= maxGuardTicks; ++si) {
        const double tSec = static_cast<double>(si) * subStepSec;
        if (tSec > safeTotal + 1e-5) break;
        const float tx = laneLeft + static_cast<float>((tSec - safeStart) / safeSpan) * trackW;
        if (tx < laneLeft - 2.0f || tx > laneRight + 2.0f) continue;

        const int mod = static_cast<int>(si % subDivs);
        if (mod == 0) {
            // 第 1 级 · 主刻度：长竖线 (14.2px) + 右侧嵌入式等宽时间数字
            setBrush(dark ? D2D1::ColorF(0.86f, 0.92f, 1.0f, 0.48f)
                          : D2D1::ColorF(0.10f, 0.16f, 0.28f, 0.46f));
            rt->DrawLine({tx, (std::max)(rulerTopY + 2.0f, rulerBottomY - 14.2f)},
                         {tx, rulerBottomY}, brush, 1.15f);

            if (monoFmt) {
                wchar_t sTxt[36]{};
                if (safeTotal >= 3600.0) {
                    if (majorStepSec < 1.0) {
                        const int64_t whole = static_cast<int64_t>(std::floor(tSec + 1e-6));
                        const double remSec = std::fmod(tSec, 60.0);
                        std::swprintf(sTxt, std::size(sTxt), L"%02lld:%02lld:%04.1f",
                                      static_cast<long long>(whole / 3600),
                                      static_cast<long long>((whole % 3600) / 60),
                                      remSec);
                    } else {
                        const int64_t ts = static_cast<int64_t>(std::llround(tSec));
                        std::swprintf(sTxt, std::size(sTxt), L"%02lld:%02lld:%02lld",
                                      static_cast<long long>(ts / 3600),
                                      static_cast<long long>((ts % 3600) / 60),
                                      static_cast<long long>(ts % 60));
                    }
                } else if (safeTotal >= 60.0) {
                    const int mins = static_cast<int>(std::floor((tSec + 1e-6) / 60.0));
                    const double remSec = std::fmod(tSec, 60.0);
                    if (majorStepSec < 1.0) {
                        std::swprintf(sTxt, std::size(sTxt), L"%02d:%04.1f", mins, remSec);
                    } else {
                        const int secs = static_cast<int>(std::round(remSec)) % 60;
                        std::swprintf(sTxt, std::size(sTxt), L"%02d:%02d", mins, secs);
                    }
                } else if (std::abs(majorStepSec - 0.25) < 1e-3) {
                    std::swprintf(sTxt, std::size(sTxt), L"%.2fs", tSec);
                } else {
                    std::swprintf(sTxt, std::size(sTxt), L"%.1fs", tSec);
                }
                const float lblW = (safeTotal >= 3600.0) ? 74.0f : 52.0f;
                const bool nearRightEdge = (tx + 4.5f + lblW > laneRight + 2.0f);
                const D2D1_RECT_F txtRc = nearRightEdge
                    ? D2D1::RectF(tx - lblW - 3.5f, rulerTopY, tx - 3.5f, rulerBottomY - 4.2f)
                    : D2D1::RectF(tx + 4.2f, rulerTopY, tx + 4.2f + lblW, rulerBottomY - 4.2f);
                monoFmt->SetTextAlignment(nearRightEdge ? DWRITE_TEXT_ALIGNMENT_TRAILING : DWRITE_TEXT_ALIGNMENT_LEADING);
                monoFmt->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
                setBrush(D2D1::ColorF(colSecondary.r, colSecondary.g, colSecondary.b, 0.88f));
                rt->DrawText(sTxt, static_cast<UINT32>(wcslen(sTxt)), monoFmt, txtRc, brush, D2D1_DRAW_TEXT_OPTIONS_CLIP);
            }
        } else if (mod == halfSub) {
            // 第 2 级 · 半格中点刻度 (如 0.5s, 1.5s)：无数字，长度 8.4px（比两侧细齿高出一倍多，清晰标定半秒中线）
            setBrush(dark ? D2D1::ColorF(0.78f, 0.86f, 0.98f, 0.35f)
                          : D2D1::ColorF(0.12f, 0.19f, 0.32f, 0.34f));
            rt->DrawLine({tx, rulerBottomY - 8.4f}, {tx, rulerBottomY}, brush, 1.05f);
        } else {
            // 第 3 级 · 十分位/四分位细微刻度 (如 0.1s..0.4s)：无数字，精致短齿 4.0px
            setBrush(dark ? D2D1::ColorF(0.74f, 0.82f, 0.95f, 0.21f)
                          : D2D1::ColorF(0.12f, 0.19f, 0.32f, 0.20f));
            rt->DrawLine({tx, rulerBottomY - 4.0f}, {tx, rulerBottomY}, brush, 0.90f);
        }
    }

    rt->PopAxisAlignedClip();
}

struct TransportConsoleLayout {
    D2D1_RECT_F podRect{};
    D2D1_RECT_F jumpStartBtn{};
    D2D1_RECT_F prevFrameBtn{};
    D2D1_RECT_F playPauseBtn{};
    D2D1_RECT_F nextFrameBtn{};
    bool hasFrameStepButtons = true;
};

TransportConsoleLayout renderIntegratedTransportConsole(
    ID2D1Factory* d2dFactory,
    ID2D1RenderTarget* rt,
    ID2D1SolidColorBrush* brush,
    IDWriteTextFormat* monoFmt,
    float centerTargetX,
    float minLeftX,
    float maxRightX,
    float topY,
    double shownKeptSec,
    double totalKeptSec,
    bool playing,
    int hoveredHitId,
    bool dark,
    const D2D1_COLOR_F& colAccent,
    const D2D1_COLOR_F& colPrimary,
    const D2D1_COLOR_F& colSecondary,
    const D2D1_COLOR_F& colBorder,
    ID2D1StrokeStyle* roundStroke) {
    TransportConsoleLayout lo{};
    if (!rt || !brush) return lo;

    auto setBrush = [&](const D2D1_COLOR_F& c) { brush->SetColor(c); };
    const std::wstring curStr = formatTimecodeMmSs(shownKeptSec);
    const std::wstring totStr = L"/ " + formatTimecodeMmSs(totalKeptSec);
    const bool wideTime = (totalKeptSec >= 3600.0);
    const float curTxtW = wideTime ? 62.0f : 52.0f;
    const float totTxtW = wideTime ? 76.0f : 64.0f;

    const float availSpan = maxRightX - minLeftX;
    lo.hasFrameStepButtons = (availSpan >= 228.0f);
    const float btnsW = lo.hasFrameStepButtons ? (22.0f + 2.0f + 22.0f + 3.0f + 28.0f + 3.0f + 22.0f) : (22.0f + 3.0f + 28.0f);
    const float totalPodW = 5.0f + btnsW + 8.0f + curTxtW + 6.0f + totTxtW + 8.0f;
    const float podLeft = std::clamp(centerTargetX - totalPodW * 0.5f, minLeftX, (std::max)(minLeftX, maxRightX - totalPodW));
    lo.podRect = D2D1::RectF(podLeft, topY, podLeft + totalPodW, topY + 28.0f);

    // 沉降微晶玻璃控制台底舱
    setBrush(dark ? D2D1::ColorF(0.048f, 0.060f, 0.094f, 0.96f)
                  : D2D1::ColorF(0.925f, 0.940f, 0.965f, 1.0f));
    rt->FillRoundedRectangle(D2D1::RoundedRect(lo.podRect, 7.5f, 7.5f), brush);
    setBrush(colBorder);
    rt->DrawRoundedRectangle(D2D1::RoundedRect(lo.podRect, 7.5f, 7.5f), brush, 0.85f);

    float bx = lo.podRect.left + 3.0f;
    const float btnT = topY + 2.2f;
    const float btnB = topY + 25.8f;
    const float cy = (btnT + btnB) * 0.5f;

    auto drawMiniKeyBg = [&](const D2D1_RECT_F& r, bool hov) -> D2D1_COLOR_F {
        if (hov) {
            fillLinearVGradRounded(
                rt, r, 5.2f,
                dark ? D2D1::ColorF(0.175f, 0.215f, 0.310f, 1.0f) : D2D1::ColorF(1.0f, 1.0f, 1.0f, 1.0f),
                dark ? D2D1::ColorF(0.138f, 0.172f, 0.254f, 1.0f) : D2D1::ColorF(0.972f, 0.984f, 1.0f, 1.0f));
            setBrush(dark ? D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.15f) : D2D1::ColorF(0.06f, 0.10f, 0.20f, 0.09f));
            rt->DrawRoundedRectangle(D2D1::RoundedRect(r, 5.2f, 5.2f), brush, 0.85f);
            return colAccent;
        }
        return dark ? D2D1::ColorF(0.80f, 0.86f, 0.95f, 0.88f) : D2D1::ColorF(0.22f, 0.28f, 0.38f, 0.88f);
    };

    // 1) [ |< ] 跳回片头
    lo.jumpStartBtn = D2D1::RectF(bx, btnT, bx + 22.0f, btnB);
    {
        const D2D1_COLOR_F ic = drawMiniKeyBg(lo.jumpStartBtn, hoveredHitId == Hit_TransportJumpStart);
        const float cx = (lo.jumpStartBtn.left + lo.jumpStartBtn.right) * 0.5f;
        setBrush(ic);
        rt->DrawLine({cx - 3.8f, cy - 4.0f}, {cx - 3.8f, cy + 4.0f}, brush, 1.45f, roundStroke);
        rt->DrawLine({cx + 3.4f, cy - 3.8f}, {cx - 1.8f, cy}, brush, 1.45f, roundStroke);
        rt->DrawLine({cx - 1.8f, cy}, {cx + 3.4f, cy + 3.8f}, brush, 1.45f, roundStroke);
    }
    bx = lo.jumpStartBtn.right + (lo.hasFrameStepButtons ? 2.0f : 3.0f);

    // 2) [ < ] 后退一帧
    if (lo.hasFrameStepButtons) {
        lo.prevFrameBtn = D2D1::RectF(bx, btnT, bx + 22.0f, btnB);
        const D2D1_COLOR_F ic = drawMiniKeyBg(lo.prevFrameBtn, hoveredHitId == Hit_TransportPrevFrame);
        const float cx = (lo.prevFrameBtn.left + lo.prevFrameBtn.right) * 0.5f;
        setBrush(ic);
        rt->DrawLine({cx + 1.8f, cy - 3.8f}, {cx - 2.4f, cy}, brush, 1.5f, roundStroke);
        rt->DrawLine({cx - 2.4f, cy}, {cx + 1.8f, cy + 3.8f}, brush, 1.5f, roundStroke);
        bx = lo.prevFrameBtn.right + 3.0f;
    }

    // 3) [ > / || ] 立体主题蓝核心播放键
    lo.playPauseBtn = D2D1::RectF(bx, btnT, bx + 28.0f, btnB);
    {
        const bool hovPlay = (hoveredHitId == Hit_PlayPauseBtn);
        const float lift = hovPlay ? 0.06f : 0.0f;
        fillLinearVGradRounded(
            rt, lo.playPauseBtn, 6.2f,
            D2D1::ColorF((std::min)(1.0f, colAccent.r + 0.08f + lift),
                         (std::min)(1.0f, colAccent.g + 0.08f + lift),
                         (std::min)(1.0f, colAccent.b + 0.08f + lift), 1.0f),
            D2D1::ColorF((std::max)(0.0f, colAccent.r - 0.04f + lift),
                         (std::max)(0.0f, colAccent.g - 0.04f + lift),
                         (std::max)(0.0f, colAccent.b - 0.02f + lift), 1.0f));
        setBrush(D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.98f));
        const float pcx = (lo.playPauseBtn.left + lo.playPauseBtn.right) * 0.5f;
        if (playing) {
            rt->FillRoundedRectangle(
                D2D1::RoundedRect(D2D1::RectF(pcx - 4.0f, cy - 4.6f, pcx - 1.3f, cy + 4.6f), 1.1f, 1.1f), brush);
            rt->FillRoundedRectangle(
                D2D1::RoundedRect(D2D1::RectF(pcx + 1.3f, cy - 4.6f, pcx + 4.0f, cy + 4.6f), 1.1f, 1.1f), brush);
        } else if (d2dFactory) {
            ComPtr<ID2D1PathGeometry> tri;
            if (SUCCEEDED(d2dFactory->CreatePathGeometry(tri.GetAddressOf())) && tri) {
                ComPtr<ID2D1GeometrySink> sink;
                if (SUCCEEDED(tri->Open(sink.GetAddressOf())) && sink) {
                    sink->BeginFigure({pcx - 2.8f, cy - 4.8f}, D2D1_FIGURE_BEGIN_FILLED);
                    sink->AddLine({pcx + 4.6f, cy});
                    sink->AddLine({pcx - 2.8f, cy + 4.8f});
                    sink->EndFigure(D2D1_FIGURE_END_CLOSED);
                    sink->Close();
                    rt->FillGeometry(tri.Get(), brush);
                }
            }
        }
    }
    bx = lo.playPauseBtn.right + 3.0f;

    // 4) [ > ] 前进一帧
    if (lo.hasFrameStepButtons) {
        lo.nextFrameBtn = D2D1::RectF(bx, btnT, bx + 22.0f, btnB);
        const D2D1_COLOR_F ic = drawMiniKeyBg(lo.nextFrameBtn, hoveredHitId == Hit_TransportNextFrame);
        const float cx = (lo.nextFrameBtn.left + lo.nextFrameBtn.right) * 0.5f;
        setBrush(ic);
        rt->DrawLine({cx - 1.8f, cy - 3.8f}, {cx + 2.4f, cy}, brush, 1.5f, roundStroke);
        rt->DrawLine({cx + 2.4f, cy}, {cx - 1.8f, cy + 3.8f}, brush, 1.5f, roundStroke);
        bx = lo.nextFrameBtn.right + 4.5f;
    } else {
        bx += 2.5f;
    }

    // 舱内微分隔线 + 高对比双色防抖时间码 (主色当前秒 / 冷灰总时长)
    setBrush(dark ? D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.09f) : D2D1::ColorF(0.08f, 0.12f, 0.22f, 0.10f));
    rt->DrawLine({bx, topY + 6.5f}, {bx, topY + 21.5f}, brush, 0.9f);
    bx += 5.5f;

    if (monoFmt) {
        monoFmt->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
        monoFmt->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_TRAILING);
        setBrush(colPrimary);
        rt->DrawText(curStr.c_str(), static_cast<UINT32>(curStr.size()), monoFmt,
                     D2D1::RectF(bx, topY, bx + curTxtW, topY + 28.0f), brush, D2D1_DRAW_TEXT_OPTIONS_CLIP);

        monoFmt->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
        setBrush(D2D1::ColorF(colSecondary.r, colSecondary.g, colSecondary.b, 0.86f));
        rt->DrawText(totStr.c_str(), static_cast<UINT32>(totStr.size()), monoFmt,
                     D2D1::RectF(bx + curTxtW + 6.0f, topY, lo.podRect.right - 5.0f, topY + 28.0f), brush, D2D1_DRAW_TEXT_OPTIONS_CLIP);
    }

    return lo;
}

void renderCinemaFilmstripAndMountainWaveformClip(
    ID2D1Factory* d2dFactory,
    ID2D1RenderTarget* rt,
    ID2D1SolidColorBrush* brush,
    IDWriteTextFormat* captionFmt,
    IDWriteTextFormat* monoFmt,
    const D2D1_RECT_F& clipRc,
    float visLaneLeft,
    float visLaneRight,
    size_t clipIndex,
    size_t totalClips,
    const StudioVideoClip& clip,
    int64_t rawTotalWaveMs,
    float lanePlayX,
    bool selected,
    bool dark,
    bool isZh,
    const std::vector<ComPtr<ID2D1Bitmap>>& filmstripBitmaps,
    const std::vector<float>& audioPeaks,
    const StudioWaveformEnvelopeConfig& waveProfile,
    ID2D1StrokeStyle* roundStroke) {
    const float visL = (std::max)(clipRc.left, visLaneLeft);
    const float visR = (std::min)(clipRc.right, visLaneRight);
    if (!rt || !brush || clipRc.right <= clipRc.left + 3.0f || visR <= visL + 1.0f || clipRc.bottom <= clipRc.top + 8.0f) {
        return;
    }

    auto setBrush = [&](const D2D1_COLOR_F& c) { brush->SetColor(c); };
    auto drawTxt = [&](const std::wstring& txt, IDWriteTextFormat* fmt, const D2D1_RECT_F& r,
                       const D2D1_COLOR_F& col,
                       DWRITE_TEXT_ALIGNMENT hAlign = DWRITE_TEXT_ALIGNMENT_LEADING) {
        if (!fmt || txt.empty() || r.right <= r.left || r.bottom <= r.top) return;
        fmt->SetTextAlignment(hAlign);
        fmt->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
        setBrush(col);
        rt->DrawText(txt.c_str(), static_cast<UINT32>(txt.size()), fmt, r, brush, D2D1_DRAW_TEXT_OPTIONS_CLIP);
    };

    const bool multiClipSel = selected && (totalClips > 1);
    const float cw = clipRc.right - clipRc.left;
    const float visCw = visR - visL;
    const float ch = clipRc.bottom - clipRc.top;
    const float splitY = clipRc.top + std::round(ch * 0.45f);
    const int64_t clipSrcSpanMs = (std::max<int64_t>)(1, clip.srcEndMs - clip.srcStartMs);
    const int64_t cDurMs = clip.keptDurationMs();
    const D2D1_RECT_F boundedClipRc = D2D1::RectF(
        (std::max)(clipRc.left, visLaneLeft - 12.0f), clipRc.top,
        (std::min)(clipRc.right, visLaneRight + 12.0f), clipRc.bottom);

    setBrush(D2D1::ColorF(0.02f, 0.04f, 0.09f, dark ? 0.36f : 0.07f));
    rt->FillRoundedRectangle(
        D2D1::RoundedRect(D2D1::RectF(boundedClipRc.left, boundedClipRc.top + 1.0f, boundedClipRc.right, boundedClipRc.bottom + 1.4f), 6.5f, 6.5f),
        brush);

    ComPtr<ID2D1RoundedRectangleGeometry> clipRoundGeo;
    ComPtr<ID2D1Layer> clipLayer;
    bool pushedGeoLayer = false;
    if (d2dFactory &&
        SUCCEEDED(d2dFactory->CreateRoundedRectangleGeometry(
            D2D1::RoundedRect(boundedClipRc, 6.5f, 6.5f), clipRoundGeo.GetAddressOf())) &&
        clipRoundGeo &&
        SUCCEEDED(rt->CreateLayer(nullptr, clipLayer.GetAddressOf())) &&
        clipLayer) {
        rt->PushLayer(D2D1::LayerParameters(boundedClipRc, clipRoundGeo.Get()), clipLayer.Get());
        pushedGeoLayer = true;
    } else {
        rt->PushAxisAlignedClip(boundedClipRc, D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);
    }

    // ── 上舱：Cinema Filmstrip 连续真实录屏缩略图画廊（仅光栅化当前屏幕可见范围，支持 24x 无限丝滑放大） ──
    const D2D1_RECT_F filmRc = D2D1::RectF(boundedClipRc.left, clipRc.top, boundedClipRc.right, splitY);
    setBrush(dark ? D2D1::ColorF(0.068f, 0.090f, 0.140f, 1.0f) : D2D1::ColorF(0.115f, 0.155f, 0.235f, 1.0f));
    rt->FillRectangle(filmRc, brush);

    if (!filmstripBitmaps.empty()) {
        constexpr float kIdealTileW = 46.0f;
        const int numTiles = (std::max)(1, static_cast<int>(std::ceil(cw / kIdealTileW)));
        const float tileW = cw / static_cast<float>(numTiles);
        const float tileH = (std::max)(1.0f, filmRc.bottom - filmRc.top);
        const float targetAspect = tileW / tileH;
        const int firstTi = std::clamp(static_cast<int>(std::floor((visL - clipRc.left) / tileW)) - 1, 0, numTiles - 1);
        const int lastTi  = std::clamp(static_cast<int>(std::ceil((visR - clipRc.left) / tileW)), 0, numTiles - 1);

        for (int ti = firstTi; ti <= lastTi; ++ti) {
            const float tx0 = clipRc.left + static_cast<float>(ti) * tileW;
            const float tx1 = (ti + 1 == numTiles) ? clipRc.right : (tx0 + tileW);
            const D2D1_RECT_F dstTile = D2D1::RectF(tx0, filmRc.top, tx1, filmRc.bottom);

            const double uClip = (static_cast<double>(ti) + 0.5) / static_cast<double>(numTiles);
            const double srcMs = static_cast<double>(clip.srcStartMs) + uClip * static_cast<double>(clipSrcSpanMs);
            const double uGlobal = std::clamp(srcMs / static_cast<double>((std::max<int64_t>)(1, rawTotalWaveMs)), 0.0, 1.0);
            const size_t fIdx = (std::min)(
                filmstripBitmaps.size() - 1,
                static_cast<size_t>(std::llround(uGlobal * static_cast<double>(filmstripBitmaps.size() - 1))));

            if (auto* bmp = filmstripBitmaps[fIdx].Get()) {
                const D2D1_SIZE_F bsz = bmp->GetSize();
                if (bsz.width > 1.0f && bsz.height > 1.0f) {
                    const float bmpAspect = bsz.width / bsz.height;
                    D2D1_RECT_F srcCrop = D2D1::RectF(0.0f, 0.0f, bsz.width, bsz.height);
                    if (bmpAspect > targetAspect) {
                        const float cropW = bsz.height * targetAspect;
                        const float padX = (bsz.width - cropW) * 0.5f;
                        srcCrop = D2D1::RectF(padX, 0.0f, padX + cropW, bsz.height);
                    } else {
                        const float cropH = bsz.width / targetAspect;
                        const float padY = (bsz.height - cropH) * 0.5f;
                        srcCrop = D2D1::RectF(0.0f, padY, bsz.width, padY + cropH);
                    }
                    rt->DrawBitmap(bmp, dstTile, 0.94f, D2D1_BITMAP_INTERPOLATION_MODE_LINEAR, srcCrop);
                }
            }
            if (ti + 1 < numTiles) {
                setBrush(D2D1::ColorF(0.02f, 0.04f, 0.08f, 0.34f));
                rt->DrawLine({tx1, filmRc.top}, {tx1, filmRc.bottom}, brush, 0.85f);
            }
        }
        setBrush(D2D1::ColorF(0.04f, 0.07f, 0.13f, dark ? 0.22f : 0.14f));
        rt->FillRectangle(filmRc, brush);
    }

    // 左上角吸左悬浮黑曜石毛玻璃胶囊 + 右上角倍速微标
    if (visCw >= 44.0f) {
        const std::wstring durStr = formatCompactDurationBadge(cDurMs / 1000.0, isZh);
        wchar_t cLbl[80]{};
        if (visCw >= 116.0f) {
            std::swprintf(cLbl, std::size(cLbl),
                          isZh ? L"片段 %zu · %s" : L"Clip %zu · %s",
                          clipIndex + 1, durStr.c_str());
        } else {
            std::swprintf(cLbl, std::size(cLbl), L"%s", durStr.c_str());
        }

        float estMetaW = 18.0f;
        for (size_t ci = 0; cLbl[ci] != L'\0'; ++ci) {
            estMetaW += (cLbl[ci] > 0x7F) ? 11.8f : 6.5f;
        }
        const bool fastSpd = std::abs(clip.speed - 1.0f) > 0.03f;
        const bool showSpdPill = (visCw >= 98.0f) || (fastSpd && visCw >= 68.0f);
        const float rightReserve = showSpdPill ? 39.0f : 6.0f;
        const float stickyL = visL + 5.0f;
        const float stickyR = visR - 5.0f;
        const float chipRight = (std::min)(stickyL + estMetaW, visR - rightReserve);
        const D2D1_RECT_F metaChipRc = D2D1::RectF(stickyL, clipRc.top + 4.0f, chipRight, splitY - 4.0f);

        setBrush(D2D1::ColorF(0.05f, 0.08f, 0.14f, 0.74f));
        rt->FillRoundedRectangle(D2D1::RoundedRect(metaChipRc, 4.5f, 4.5f), brush);
        setBrush(D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.18f));
        rt->DrawRoundedRectangle(D2D1::RoundedRect(metaChipRc, 4.5f, 4.5f), brush, 0.75f);

        const float dotCy = (metaChipRc.top + metaChipRc.bottom) * 0.5f;
        setBrush(selected
            ? D2D1::ColorF(0.38f, 0.70f, 1.0f, 1.0f)
            : D2D1::ColorF(0.72f, 0.82f, 0.96f, 0.90f));
        rt->FillEllipse(D2D1::Ellipse({metaChipRc.left + 6.5f, dotCy}, 2.2f, 2.2f), brush);

        drawTxt(cLbl, captionFmt,
                D2D1::RectF(metaChipRc.left + 12.0f, metaChipRc.top - 0.5f, metaChipRc.right - 3.5f, metaChipRc.bottom + 0.5f),
                D2D1::ColorF(0.97f, 0.98f, 1.0f, 0.96f));

        if (showSpdPill) {
            wchar_t spdChip[20]{};
            std::swprintf(spdChip, std::size(spdChip), L"%.1fx", clip.speed);
            const D2D1_RECT_F pillRc = D2D1::RectF(stickyR - 30.0f, clipRc.top + 4.0f, stickyR, splitY - 4.0f);
            setBrush(fastSpd
                ? D2D1::ColorF(0.14f, 0.44f, 0.94f, 0.86f)
                : D2D1::ColorF(0.05f, 0.08f, 0.14f, 0.70f));
            rt->FillRoundedRectangle(D2D1::RoundedRect(pillRc, 4.5f, 4.5f), brush);
            setBrush(D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.18f));
            rt->DrawRoundedRectangle(D2D1::RoundedRect(pillRc, 4.5f, 4.5f), brush, 0.75f);
            drawTxt(spdChip, monoFmt, pillRc, D2D1::ColorF(0.96f, 0.98f, 1.0f, 0.96f), DWRITE_TEXT_ALIGNMENT_CENTER);
        }
    }

    // ── 下舱：Sculpted Acoustic Mountain Waveform（随时间轴放大自动展开亚秒级声波细节） ──
    const D2D1_RECT_F waveBox = D2D1::RectF(boundedClipRc.left, splitY, boundedClipRc.right, clipRc.bottom);
    fillLinearVGradRounded(
        rt, waveBox, 0.0f,
        dark ? D2D1::ColorF(0.045f, 0.068f, 0.118f, 1.0f) : D2D1::ColorF(0.948f, 0.968f, 0.994f, 1.0f),
        dark ? D2D1::ColorF(0.032f, 0.048f, 0.088f, 1.0f) : D2D1::ColorF(0.898f, 0.932f, 0.980f, 1.0f));

    setBrush(dark ? D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.10f) : D2D1::ColorF(0.08f, 0.16f, 0.30f, 0.12f));
    rt->DrawLine({boundedClipRc.left, splitY}, {boundedClipRc.right, splitY}, brush, 0.85f);

    const float waveLeft = visL + 0.5f;
    const float waveRight = visR - 0.5f;
    const float baseY = clipRc.bottom - 1.5f;
    const float maxMountainH = (std::max)(6.0f, (baseY - splitY) - 4.0f);

    if (waveRight > waveLeft + 4.0f) {
        if (waveProfile.hasAudio && !audioPeaks.empty() && d2dFactory) {
            constexpr float kStepPx = 2.0f;
            const float spanPx = waveRight - waveLeft;
            const int numPts = (std::max)(3, static_cast<int>(std::ceil(spanPx / kStepPx)) + 1);
            std::vector<float> rawBody(static_cast<size_t>(numPts), 0.0f);
            std::vector<float> rawPeak(static_cast<size_t>(numPts), 0.0f);

            for (int pi = 0; pi < numPts; ++pi) {
                const float xCur = waveLeft + (static_cast<float>(pi) / static_cast<float>(numPts - 1)) * spanPx;
                const float halfStep = (spanPx / static_cast<float>(numPts - 1)) * 0.5f;
                const double u0 = std::clamp<double>(((xCur - halfStep) - clipRc.left) / (std::max)(1.0f, cw), 0.0, 1.0);
                const double u1 = std::clamp<double>(((xCur + halfStep) - clipRc.left) / (std::max)(1.0f, cw), u0, 1.0);
                const int64_t sMs = clip.srcStartMs + static_cast<int64_t>(std::llround(u0 * clipSrcSpanMs));
                const int64_t eMs = clip.srcStartMs + static_cast<int64_t>(std::llround(u1 * clipSrcSpanMs));
                StudioTimelineAnalyzer::sampleSculptedWaveformColumn(
                    audioPeaks, rawTotalWaveMs, sMs, eMs, waveProfile,
                    rawBody[static_cast<size_t>(pi)], rawPeak[static_cast<size_t>(pi)]);
            }

            // 灵敏 3-Tap 高斯声学包络平滑（既抗锯齿顺滑，又完整保留真实节拍瞬态峰谷）
            std::vector<float> smBody(static_cast<size_t>(numPts), 0.0f);
            std::vector<float> smPeak(static_cast<size_t>(numPts), 0.0f);
            for (int pi = 0; pi < numPts; ++pi) {
                auto atB = [&](int k) { return rawBody[static_cast<size_t>(std::clamp(k, 0, numPts - 1))]; };
                auto atP = [&](int k) { return rawPeak[static_cast<size_t>(std::clamp(k, 0, numPts - 1))]; };
                if (rawBody[static_cast<size_t>(pi)] <= 0.008f &&
                    atB(pi - 1) <= 0.008f && atB(pi + 1) <= 0.008f) {
                    smBody[static_cast<size_t>(pi)] = 0.0f;
                    smPeak[static_cast<size_t>(pi)] = 0.0f;
                } else {
                    smBody[static_cast<size_t>(pi)] =
                        0.20f * atB(pi - 1) + 0.60f * atB(pi) + 0.20f * atB(pi + 1);
                    smPeak[static_cast<size_t>(pi)] = (std::max)(
                        smBody[static_cast<size_t>(pi)],
                        0.22f * atP(pi - 1) + 0.56f * atP(pi) + 0.22f * atP(pi + 1));
                }
            }

            auto xAt = [&](int pi) -> float {
                return waveLeft + (static_cast<float>(pi) / static_cast<float>(numPts - 1)) * spanPx;
            };
            auto bodyYAt = [&](int pi) -> float {
                const float e = smBody[static_cast<size_t>(pi)];
                if (e <= 0.008f) return baseY;
                return baseY - std::clamp(1.6f + e * (maxMountainH - 1.6f), 1.6f, maxMountainH);
            };
            auto peakYAt = [&](int pi) -> float {
                const float e = smPeak[static_cast<size_t>(pi)];
                if (e <= 0.008f) return baseY;
                return baseY - std::clamp(2.2f + e * (maxMountainH - 1.0f), 2.2f, maxMountainH + 1.5f);
            };

            ComPtr<ID2D1PathGeometry> crestGeo;
            ComPtr<ID2D1PathGeometry> bodyGeo;
            ComPtr<ID2D1PathGeometry> ridgeGeo;
            if (SUCCEEDED(d2dFactory->CreatePathGeometry(crestGeo.GetAddressOf())) && crestGeo) {
                ComPtr<ID2D1GeometrySink> sk;
                if (SUCCEEDED(crestGeo->Open(sk.GetAddressOf())) && sk) {
                    sk->BeginFigure({xAt(0), baseY}, D2D1_FIGURE_BEGIN_FILLED);
                    for (int pi = 0; pi < numPts; ++pi) sk->AddLine({xAt(pi), peakYAt(pi)});
                    sk->AddLine({xAt(numPts - 1), baseY});
                    sk->EndFigure(D2D1_FIGURE_END_CLOSED);
                    sk->Close();
                }
            }
            if (SUCCEEDED(d2dFactory->CreatePathGeometry(bodyGeo.GetAddressOf())) && bodyGeo) {
                ComPtr<ID2D1GeometrySink> sk;
                if (SUCCEEDED(bodyGeo->Open(sk.GetAddressOf())) && sk) {
                    sk->BeginFigure({xAt(0), baseY}, D2D1_FIGURE_BEGIN_FILLED);
                    for (int pi = 0; pi < numPts; ++pi) sk->AddLine({xAt(pi), bodyYAt(pi)});
                    sk->AddLine({xAt(numPts - 1), baseY});
                    sk->EndFigure(D2D1_FIGURE_END_CLOSED);
                    sk->Close();
                }
            }
            if (SUCCEEDED(d2dFactory->CreatePathGeometry(ridgeGeo.GetAddressOf())) && ridgeGeo) {
                ComPtr<ID2D1GeometrySink> sk;
                if (SUCCEEDED(ridgeGeo->Open(sk.GetAddressOf())) && sk) {
                    bool inRun = false;
                    for (int pi = 0; pi < numPts; ++pi) {
                        const bool active = (smBody[static_cast<size_t>(pi)] > 0.012f);
                        if (active && !inRun) {
                            const int pStart = (std::max)(0, pi - 1);
                            sk->BeginFigure({xAt(pStart), bodyYAt(pStart)}, D2D1_FIGURE_BEGIN_HOLLOW);
                            if (pStart != pi) sk->AddLine({xAt(pi), bodyYAt(pi)});
                            inRun = true;
                        } else if (active && inRun) {
                            sk->AddLine({xAt(pi), bodyYAt(pi)});
                        } else if (!active && inRun) {
                            sk->AddLine({xAt(pi), baseY});
                            sk->EndFigure(D2D1_FIGURE_END_OPEN);
                            inRun = false;
                        }
                    }
                    if (inRun) {
                        sk->EndFigure(D2D1_FIGURE_END_OPEN);
                    }
                    sk->Close();
                }
            }

            // 创建已播放区与未播放区垂直山峦渐变画刷
            auto makeMountainGrad = [&](const D2D1_COLOR_F& topCol, const D2D1_COLOR_F& botCol) {
                ComPtr<ID2D1LinearGradientBrush> gb;
                D2D1_GRADIENT_STOP stops[2] = {{0.0f, topCol}, {1.0f, botCol}};
                ComPtr<ID2D1GradientStopCollection> sc;
                if (SUCCEEDED(rt->CreateGradientStopCollection(
                        stops, 2, D2D1_GAMMA_2_2, D2D1_EXTEND_MODE_CLAMP, sc.GetAddressOf())) && sc) {
                    rt->CreateLinearGradientBrush(
                        D2D1::LinearGradientBrushProperties({waveLeft, baseY - maxMountainH}, {waveLeft, baseY}),
                        sc.Get(), gb.GetAddressOf());
                }
                return gb;
            };

            const auto playedGrad = makeMountainGrad(
                dark ? D2D1::ColorF(0.24f, 0.62f, 1.00f, 0.86f) : D2D1::ColorF(0.14f, 0.44f, 0.95f, 0.80f),
                dark ? D2D1::ColorF(0.12f, 0.36f, 0.82f, 0.18f) : D2D1::ColorF(0.22f, 0.56f, 0.98f, 0.16f));
            const auto futureGrad = makeMountainGrad(
                dark ? D2D1::ColorF(0.46f, 0.58f, 0.76f, 0.48f) : D2D1::ColorF(0.32f, 0.48f, 0.72f, 0.46f),
                dark ? D2D1::ColorF(0.32f, 0.42f, 0.58f, 0.10f) : D2D1::ColorF(0.42f, 0.56f, 0.78f, 0.10f));

            auto paintHalfZone = [&](float zLeft, float zRight, bool isPlayed) {
                if (zRight <= zLeft + 0.2f) return;
                rt->PushAxisAlignedClip(
                    D2D1::RectF(zLeft, splitY, zRight, clipRc.bottom),
                    D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);
                if (crestGeo) {
                    setBrush(isPlayed
                        ? (dark ? D2D1::ColorF(0.38f, 0.74f, 1.0f, 0.20f) : D2D1::ColorF(0.18f, 0.52f, 0.96f, 0.18f))
                        : (dark ? D2D1::ColorF(0.56f, 0.68f, 0.84f, 0.12f) : D2D1::ColorF(0.36f, 0.50f, 0.72f, 0.12f)));
                    rt->FillGeometry(crestGeo.Get(), brush);
                }
                if (bodyGeo) {
                    auto* g = isPlayed ? playedGrad.Get() : futureGrad.Get();
                    if (g) {
                        rt->FillGeometry(bodyGeo.Get(), g);
                    } else {
                        setBrush(D2D1::ColorF(0.22f, 0.50f, 0.94f, isPlayed ? 0.65f : 0.32f));
                        rt->FillGeometry(bodyGeo.Get(), brush);
                    }
                }
                if (ridgeGeo) {
                    setBrush(isPlayed
                        ? (dark ? D2D1::ColorF(0.58f, 0.86f, 1.0f, 0.96f) : D2D1::ColorF(0.10f, 0.38f, 0.90f, 0.94f))
                        : (dark ? D2D1::ColorF(0.64f, 0.76f, 0.92f, 0.64f) : D2D1::ColorF(0.28f, 0.44f, 0.68f, 0.66f)));
                    rt->DrawGeometry(ridgeGeo.Get(), brush, isPlayed ? 1.40f : 1.20f, roundStroke);
                }
                rt->PopAxisAlignedClip();
            };

            const float splitPlayX = std::clamp(lanePlayX, boundedClipRc.left, boundedClipRc.right);
            paintHalfZone(boundedClipRc.left, splitPlayX, true);
            paintHalfZone(splitPlayX, boundedClipRc.right, false);

            // 智能静音气口识别引导 (Smart Silence Cut Guide)：自动标出 >= 0.45s 的空白停顿段方便一键拆分剪除
            D2D1_STROKE_STYLE_PROPERTIES dsp = D2D1::StrokeStyleProperties();
            dsp.dashStyle = D2D1_DASH_STYLE_DASH;
            ComPtr<ID2D1StrokeStyle> dashStyle;
            d2dFactory->CreateStrokeStyle(dsp, nullptr, 0, dashStyle.GetAddressOf());

            int silStart = -1;
            for (int pi = 0; pi <= numPts; ++pi) {
                const bool isSil = (pi < numPts) && (smBody[static_cast<size_t>(pi)] <= 0.012f);
                if (isSil && silStart < 0) {
                    silStart = pi;
                } else if (!isSil && silStart >= 0) {
                    const int silEnd = pi - 1;
                    const float sx0 = xAt(silStart);
                    const float sx1 = xAt(silEnd);
                    const float silPxW = sx1 - sx0;
                    const double silDurSec =
                        (static_cast<double>(silPxW) / (std::max)(1.0f, cw)) *
                        (static_cast<double>(clipSrcSpanMs) / 1000.0);
                    if (silPxW >= 10.0f) {
                        setBrush(dark ? D2D1::ColorF(0.56f, 0.68f, 0.86f, 0.24f)
                                      : D2D1::ColorF(0.26f, 0.38f, 0.56f, 0.24f));
                        rt->DrawLine({sx0 + 2.0f, baseY - 1.0f}, {sx1 - 2.0f, baseY - 1.0f},
                                     brush, 0.95f, dashStyle.Get());
                    }
                    if (silDurSec >= 0.45 && silPxW >= 68.0f) {
                        const std::wstring silDurTxt = formatCompactDurationBadge(silDurSec, isZh);
                        wchar_t silLbl[48]{};
                        std::swprintf(silLbl, std::size(silLbl),
                                      isZh ? L"静音 %s" : L"Silence %s", silDurTxt.c_str());
                        const float badgeW = (std::min)(silPxW - 10.0f, isZh ? 76.0f : 86.0f);
                        const float midX = (sx0 + sx1) * 0.5f;
                        const float midWaveY = (splitY + baseY) * 0.5f;
                        const D2D1_RECT_F silPill = D2D1::RectF(
                            midX - badgeW * 0.5f, midWaveY - 8.0f, midX + badgeW * 0.5f, midWaveY + 8.0f);
                        setBrush(dark ? D2D1::ColorF(0.10f, 0.15f, 0.24f, 0.76f)
                                      : D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.78f));
                        rt->FillRoundedRectangle(D2D1::RoundedRect(silPill, 4.0f, 4.0f), brush);
                        setBrush(dark ? D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.10f)
                                      : D2D1::ColorF(0.14f, 0.24f, 0.42f, 0.12f));
                        rt->DrawRoundedRectangle(D2D1::RoundedRect(silPill, 4.0f, 4.0f), brush, 0.75f);
                        drawTxt(silLbl, captionFmt, silPill,
                                dark ? D2D1::ColorF(0.68f, 0.78f, 0.92f, 0.85f)
                                     : D2D1::ColorF(0.30f, 0.40f, 0.56f, 0.86f),
                                DWRITE_TEXT_ALIGNMENT_CENTER);
                    }
                    silStart = -1;
                }
            }
        } else {
            const float midWaveY = (splitY + baseY) * 0.5f;
            setBrush(dark ? D2D1::ColorF(0.56f, 0.68f, 0.86f, 0.16f)
                          : D2D1::ColorF(0.24f, 0.36f, 0.54f, 0.16f));
            rt->DrawLine({waveLeft + 4.0f, midWaveY}, {waveRight - 4.0f, midWaveY}, brush, 0.85f);
            if (cw >= 120.0f) {
                drawTxt(isZh ? L"无音频轨道 / 静音片段" : L"Muted / No Audio Track",
                        captionFmt, waveBox,
                        dark ? D2D1::ColorF(0.62f, 0.70f, 0.82f, 0.54f)
                             : D2D1::ColorF(0.32f, 0.40f, 0.52f, 0.56f),
                        DWRITE_TEXT_ALIGNMENT_CENTER);
            }
        }
    }

    if (pushedGeoLayer) {
        rt->PopLayer();
    } else {
        rt->PopAxisAlignedClip();
    }

    // 卡片顶部镜面内亮丝与精密外边框
    setBrush(D2D1::ColorF(1.0f, 1.0f, 1.0f, dark ? 0.14f : 0.35f));
    rt->DrawLine({boundedClipRc.left + 7.0f, boundedClipRc.top + 0.8f},
                 {boundedClipRc.right - 7.0f, boundedClipRc.top + 0.8f}, brush, 0.85f);
    if (multiClipSel) {
        setBrush(dark ? D2D1::ColorF(0.36f, 0.66f, 1.0f, 0.96f) : D2D1::ColorF(0.14f, 0.40f, 0.94f, 0.92f));
        rt->DrawRoundedRectangle(D2D1::RoundedRect(boundedClipRc, 6.5f, 6.5f), brush, 1.5f);
    } else {
        setBrush(dark ? D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.15f) : D2D1::ColorF(0.10f, 0.18f, 0.32f, 0.18f));
        rt->DrawRoundedRectangle(D2D1::RoundedRect(boundedClipRc, 6.5f, 6.5f), brush, 1.0f);
    }
}

} // namespace

cv::Mat StudioPreviewWindow::renderTimelineStripPreviewForTest(
    const std::vector<cv::Mat>& deckBgr,
    const std::vector<float>& audioPeaks,
    double durationSec,
    double playheadSec,
    bool dark,
    bool isZh) {
    ComPtr<ID2D1Factory> d2d;
    ComPtr<IDWriteFactory> dw;
    if (FAILED(D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, d2d.GetAddressOf())) || !d2d) return {};
    if (FAILED(DWriteCreateFactory(
            DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory),
            reinterpret_cast<IUnknown**>(dw.GetAddressOf()))) || !dw) {
        return {};
    }

    const wchar_t* uiFont = tools3000::core::resolveInstalledUiFontFamily(dw.Get());
    const wchar_t* monoFont = tools3000::core::resolveInstalledMonoFontFamily(dw.Get());
    ComPtr<IDWriteTextFormat> capFmt, monoFmt;
    dw->CreateTextFormat(uiFont, nullptr, DWRITE_FONT_WEIGHT_MEDIUM,
                         DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, 11.9f, L"zh-CN", capFmt.GetAddressOf());
    dw->CreateTextFormat(monoFont, nullptr, DWRITE_FONT_WEIGHT_SEMI_BOLD,
                         DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, 11.9f, L"en-US", monoFmt.GetAddressOf());

    constexpr float kScale = 1.5f;
    constexpr int kLogW = 980;
    constexpr int kLogH = 142;
    constexpr int kPhysW = static_cast<int>(kLogW * kScale);
    constexpr int kPhysH = static_cast<int>(kLogH * kScale);

    BITMAPINFO bmi{};
    bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bmi.bmiHeader.biWidth = kPhysW;
    bmi.bmiHeader.biHeight = -kPhysH;
    bmi.bmiHeader.biPlanes = 1;
    bmi.bmiHeader.biBitCount = 32;
    bmi.bmiHeader.biCompression = BI_RGB;

    HDC memDc = CreateCompatibleDC(nullptr);
    if (!memDc) return {};
    void* bits = nullptr;
    HBITMAP hBmp = CreateDIBSection(memDc, &bmi, DIB_RGB_COLORS, &bits, nullptr, 0);
    if (!hBmp || !bits) {
        if (hBmp) DeleteObject(hBmp);
        DeleteDC(memDc);
        return {};
    }
    HGDIOBJ oldBmp = SelectObject(memDc, hBmp);

    D2D1_RENDER_TARGET_PROPERTIES props = D2D1::RenderTargetProperties(
        D2D1_RENDER_TARGET_TYPE_DEFAULT,
        D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED),
        96.0f * kScale, 96.0f * kScale);
    ComPtr<ID2D1DCRenderTarget> rt;
    cv::Mat outBgr;
    if (SUCCEEDED(d2d->CreateDCRenderTarget(&props, rt.GetAddressOf())) && rt) {
        RECT bindRc{0, 0, kPhysW, kPhysH};
        if (SUCCEEDED(rt->BindDC(memDc, &bindRc))) {
            std::vector<ComPtr<ID2D1Bitmap>> strips;
            D2D1_BITMAP_PROPERTIES bp = D2D1::BitmapProperties(
                D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_IGNORE), 96.0f, 96.0f);
            const size_t stripCnt = (std::min<size_t>)(32, deckBgr.size());
            for (size_t si = 0; si < stripCnt; ++si) {
                const size_t idx = (stripCnt <= 1) ? 0 : ((si * (deckBgr.size() - 1)) / (stripCnt - 1));
                const cv::Mat& sb = deckBgr[idx];
                if (sb.empty()) continue;
                const int sw = std::clamp(static_cast<int>(std::round(64.0 * sb.cols / (std::max)(1, sb.rows))), 48, 136);
                cv::Mat rsz, bgra;
                cv::resize(sb, rsz, cv::Size(sw, 64), 0.0, 0.0, cv::INTER_AREA);
                cv::cvtColor(rsz, bgra, cv::COLOR_BGR2BGRA);
                ComPtr<ID2D1Bitmap> b;
                if (SUCCEEDED(rt->CreateBitmap(D2D1::SizeU(bgra.cols, bgra.rows), bgra.data,
                                               static_cast<UINT32>(bgra.step[0]), &bp, b.GetAddressOf())) && b) {
                    strips.push_back(std::move(b));
                }
            }

            const auto waveProf = StudioTimelineAnalyzer::analyzeWaveformProfile(audioPeaks);
            const ComPtr<ID2D1StrokeStyle> roundSt = createRoundStrokeStyle(d2d.Get());
            const double safeDur = (std::max)(0.2, durationSec);
            const int64_t totalMs = static_cast<int64_t>(std::llround(safeDur * 1000.0));
            StudioVideoClip singleClip{};
            singleClip.srcStartMs = 0;
            singleClip.srcEndMs = totalMs;
            singleClip.speed = 1.0f;

            const D2D1_COLOR_F colPrimary = dark ? D2D1::ColorF(0.96f, 0.97f, 0.99f, 1.0f) : D2D1::ColorF(0.059f, 0.090f, 0.165f, 1.0f);
            const D2D1_COLOR_F colSecondary = dark ? D2D1::ColorF(0.62f, 0.68f, 0.78f, 1.0f) : D2D1::ColorF(0.32f, 0.38f, 0.47f, 1.0f);
            const D2D1_COLOR_F colBorder = dark ? D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.10f) : D2D1::ColorF(0.059f, 0.090f, 0.165f, 0.11f);
            const D2D1_COLOR_F colAccent = D2D1::ColorF(0.15f, 0.44f, 0.96f, 1.0f);

            rt->BeginDraw();
            rt->Clear(dark ? D2D1::ColorF(0.092f, 0.114f, 0.165f, 1.0f) : D2D1::ColorF(1.0f, 1.0f, 1.0f, 1.0f));
            ComPtr<ID2D1SolidColorBrush> br;
            rt->CreateSolidColorBrush(D2D1::ColorF(1, 1, 1, 1), br.GetAddressOf());
            if (br) {
                // 1) 左·中·右黄金三段式控制栏预览（左翼创作三舱 + 居中走带中枢 + 右翼视野缩放舱）
                auto drawPreviewPod = [&](float pl, float pr) {
                    const D2D1_RECT_F prc = D2D1::RectF(pl, 6.0f, pr, 34.0f);
                    br->SetColor(dark ? D2D1::ColorF(0.048f, 0.060f, 0.094f, 0.96f) : D2D1::ColorF(0.925f, 0.940f, 0.965f, 1.0f));
                    rt->FillRoundedRectangle(D2D1::RoundedRect(prc, 7.5f, 7.5f), br.Get());
                    br->SetColor(colBorder);
                    rt->DrawRoundedRectangle(D2D1::RoundedRect(prc, 7.5f, 7.5f), br.Get(), 0.85f);
                };
                auto drawPreviewDiv = [&](float dx) {
                    br->SetColor(dark ? D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.08f) : D2D1::ColorF(0.08f, 0.12f, 0.22f, 0.09f));
                    rt->DrawLine({dx, 12.0f}, {dx, 28.0f}, br.Get(), 0.9f);
                };
                drawPreviewPod(10.0f, 211.0f);
                drawPreviewDiv(85.5f);
                drawPreviewDiv(146.5f);
                drawPreviewPod(218.0f, 340.5f);
                drawPreviewDiv(278.0f);
                drawPreviewPod(347.5f, 472.5f);
                drawPreviewDiv(407.5f);
                if (capFmt && monoFmt) {
                    capFmt->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
                    capFmt->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
                    monoFmt->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
                    monoFmt->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
                    const D2D1_COLOR_F icCol = dark ? D2D1::ColorF(0.84f, 0.89f, 0.96f, 1.0f) : D2D1::ColorF(0.20f, 0.26f, 0.36f, 1.0f);
                    br->SetColor(icCol);
                    drawScissorsIcon(rt.Get(), br.Get(), 26.0f, 20.0f, roundSt.Get());
                    drawCrispSpeedIcon(rt.Get(), br.Get(), 162.0f, 20.0f, roundSt.Get());
                    drawCrispPenIcon(d2d.Get(), rt.Get(), br.Get(), 363.5f, 20.0f, roundSt.Get());
                    drawCrispCameraFocusIcon(rt.Get(), br.Get(), 423.5f, 20.0f, roundSt.Get());
                    br->SetColor(D2D1::ColorF(icCol.r, icCol.g, icCol.b, 0.36f));
                    drawCrispTrashIcon(rt.Get(), br.Get(), 101.0f, 20.0f, roundSt.Get());
                    drawCrispUndoRedoIcon(d2d.Get(), rt.Get(), br.Get(), 234.0f, 20.0f, true, roundSt.Get());
                    drawCrispUndoRedoIcon(d2d.Get(), rt.Get(), br.Get(), 293.0f, 20.0f, false, roundSt.Get());

                    br->SetColor(colPrimary);
                    const wchar_t* s1 = isZh ? L"拆分(S)" : L"Split(S)";
                    const wchar_t* s2 = isZh ? L"标注" : L"Mark";
                    const wchar_t* s3 = isZh ? L"运镜" : L"Zoom";
                    rt->DrawText(s1, static_cast<UINT32>(wcslen(s1)), capFmt.Get(), D2D1::RectF(34.0f, 8.0f, 84.0f, 32.0f), br.Get());
                    rt->DrawText(L"1.0x", 4, monoFmt.Get(), D2D1::RectF(170.0f, 8.0f, 209.0f, 32.0f), br.Get());
                    rt->DrawText(s2, static_cast<UINT32>(wcslen(s2)), capFmt.Get(), D2D1::RectF(371.0f, 8.0f, 406.0f, 32.0f), br.Get());
                    rt->DrawText(s3, static_cast<UINT32>(wcslen(s3)), capFmt.Get(), D2D1::RectF(431.0f, 8.0f, 470.0f, 32.0f), br.Get());

                    br->SetColor(D2D1::ColorF(colPrimary.r, colPrimary.g, colPrimary.b, 0.35f));
                    const wchar_t* d1 = isZh ? L"删除" : L"Delete";
                    const wchar_t* d2 = isZh ? L"撤销" : L"Undo";
                    const wchar_t* d3 = isZh ? L"重做" : L"Redo";
                    rt->DrawText(d1, static_cast<UINT32>(wcslen(d1)), capFmt.Get(), D2D1::RectF(109.0f, 8.0f, 145.0f, 32.0f), br.Get());
                    rt->DrawText(d2, static_cast<UINT32>(wcslen(d2)), capFmt.Get(), D2D1::RectF(242.0f, 8.0f, 276.0f, 32.0f), br.Get());
                    rt->DrawText(d3, static_cast<UINT32>(wcslen(d3)), capFmt.Get(), D2D1::RectF(301.0f, 8.0f, 338.0f, 32.0f), br.Get());
                }

                const float zoomPodLeft = kLogW - 166.0f;
                const float zoomPodRight = kLogW - 12.0f;
                renderIntegratedTransportConsole(
                    d2d.Get(), rt.Get(), br.Get(), monoFmt.Get(),
                    (476.0f + zoomPodLeft) * 0.5f, 480.0f, zoomPodLeft - 8.0f, 6.0f,
                    playheadSec, safeDur, false, 0, dark,
                    colAccent, colPrimary, colSecondary, colBorder, roundSt.Get());

                drawPreviewPod(zoomPodLeft, zoomPodRight);
                drawPreviewDiv(zoomPodLeft + 48.0f);
                if (capFmt) {
                    capFmt->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
                    const wchar_t* zTxt = isZh ? L"全览" : L"Fit";
                    br->SetColor(colSecondary);
                    rt->DrawText(zTxt, static_cast<UINT32>(wcslen(zTxt)), capFmt.Get(),
                                 D2D1::RectF(zoomPodLeft + 3.0f, 6.0f, zoomPodLeft + 47.0f, 34.0f), br.Get());
                }
                // [-] ──o── [+]
                br->SetColor(colSecondary);
                rt->DrawLine({zoomPodLeft + 56.5f, 20.0f}, {zoomPodLeft + 63.5f, 20.0f}, br.Get(), 1.45f, roundSt.Get());
                const D2D1_RECT_F zTrk = D2D1::RectF(zoomPodLeft + 74.0f, 18.7f, zoomPodRight - 28.0f, 21.3f);
                br->SetColor(dark ? D2D1::ColorF(1, 1, 1, 0.12f) : D2D1::ColorF(0.08f, 0.14f, 0.24f, 0.12f));
                rt->FillRoundedRectangle(D2D1::RoundedRect(zTrk, 1.3f, 1.3f), br.Get());
                br->SetColor(D2D1::ColorF(1, 1, 1, 1));
                rt->FillEllipse(D2D1::Ellipse({zTrk.left, 20.0f}, 4.4f, 4.4f), br.Get());
                br->SetColor(colAccent);
                rt->DrawEllipse(D2D1::Ellipse({zTrk.left, 20.0f}, 4.4f, 4.4f), br.Get(), 1.3f);
                br->SetColor(colSecondary);
                const float plusCx = zoomPodRight - 12.5f;
                rt->DrawLine({plusCx - 3.6f, 20.0f}, {plusCx + 3.6f, 20.0f}, br.Get(), 1.45f, roundSt.Get());
                rt->DrawLine({plusCx, 16.4f}, {plusCx, 23.6f}, br.Get(), 1.45f, roundSt.Get());

                br->SetColor(colBorder);
                rt->DrawLine({10.0f, 38.0f}, {static_cast<float>(kLogW) - 10.0f, 38.0f}, br.Get(), 0.85f);

                // 2) 瑞士腕表级三级长·中·短精密游标卡尺刻度
                const float laneLeft = 80.0f;
                const float laneRight = kLogW - 14.0f;
                renderBroadcastVernierTimeRuler(
                    rt.Get(), br.Get(), monoFmt.Get(),
                    laneLeft, laneRight, 39.0f, 59.5f,
                    safeDur, 0.0, safeDur, dark, colSecondary);

                // 3) 电影胶片画廊 + 雕刻声学山峦主轨
                const D2D1_RECT_F laneRc = D2D1::RectF(laneLeft, 62.0f, laneRight, 126.0f);
                br->SetColor(dark ? D2D1::ColorF(0.036f, 0.048f, 0.076f, 0.94f) : D2D1::ColorF(0.928f, 0.942f, 0.964f, 0.92f));
                rt->FillRoundedRectangle(D2D1::RoundedRect(laneRc, 7.5f, 7.5f), br.Get());

                if (capFmt) {
                    capFmt->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
                    capFmt->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
                    br->SetColor(colSecondary);
                    const wchar_t* tLbl = isZh ? L"视频/音轨" : L"Video/Aud";
                    rt->DrawText(tLbl, static_cast<UINT32>(wcslen(tLbl)), capFmt.Get(),
                                 D2D1::RectF(12.0f, laneRc.top, 74.0f, laneRc.bottom), br.Get());
                }

                const D2D1_RECT_F clipRc = D2D1::RectF(laneRc.left + 1.5f, laneRc.top + 1.5f, laneRc.right - 1.5f, laneRc.bottom - 1.5f);
                const float playX = laneRc.left + (laneRc.right - laneRc.left) *
                    static_cast<float>(std::clamp(playheadSec / safeDur, 0.0, 1.0));
                renderCinemaFilmstripAndMountainWaveformClip(
                    d2d.Get(), rt.Get(), br.Get(), capFmt.Get(), monoFmt.Get(),
                    clipRc, laneLeft, laneRight, 0, 1, singleClip, totalMs, playX, false, dark, isZh,
                    strips, audioPeaks, waveProf, roundSt.Get());

                br->SetColor(D2D1::ColorF(0.88f, 0.14f, 0.28f, 0.98f));
                rt->DrawLine({playX, laneRc.top - 4.0f}, {playX, laneRc.bottom + 4.0f}, br.Get(), 1.4f);
            }
            rt->EndDraw();
            cv::Mat bgra(kPhysH, kPhysW, CV_8UC4, bits);
            cv::cvtColor(bgra, outBgr, cv::COLOR_BGRA2BGR);
        }
    }

    SelectObject(memDc, oldBmp);
    DeleteObject(hBmp);
    DeleteDC(memDc);
    return outBgr;
}

void StudioPreviewWindow::renderPreparingOverlay(
    float W, float H, bool dark, bool isZh, ID2D1SolidColorBrush* brush) {
    if (!m_renderTarget || !brush) return;

    const auto accentRgb = tools3000::core::NativeThemePalette::getEffectiveAccentColorRGB();
    const D2D1_COLOR_F colAccent = D2D1::ColorF(accentRgb.r, accentRgb.g, accentRgb.b, 1.0f);
    const D2D1_COLOR_F colPrimary = dark ? D2D1::ColorF(0.96f, 0.97f, 0.99f, 1.0f) : D2D1::ColorF(0.059f, 0.090f, 0.165f, 1.0f);
    const D2D1_COLOR_F colSecondary = dark ? D2D1::ColorF(0.62f, 0.68f, 0.78f, 1.0f) : D2D1::ColorF(0.34f, 0.40f, 0.50f, 1.0f);
    const D2D1_COLOR_F colCardBg = dark ? D2D1::ColorF(0.090f, 0.114f, 0.168f, 0.98f) : D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.98f);
    const D2D1_COLOR_F colBorder = dark ? D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.11f) : D2D1::ColorF(0.06f, 0.10f, 0.20f, 0.10f);

    auto setBrush = [&](const D2D1_COLOR_F& c) { brush->SetColor(c); };
    auto drawTxt = [&](const std::wstring& txt, IDWriteTextFormat* fmt, const D2D1_RECT_F& r,
                       const D2D1_COLOR_F& col,
                       DWRITE_TEXT_ALIGNMENT hAlign = DWRITE_TEXT_ALIGNMENT_CENTER) {
        if (!fmt || txt.empty() || r.right <= r.left || r.bottom <= r.top) return;
        fmt->SetTextAlignment(hAlign);
        fmt->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
        setBrush(col);
        m_renderTarget->DrawText(txt.c_str(), static_cast<UINT32>(txt.size()), fmt, r, brush, D2D1_DRAW_TEXT_OPTIONS_CLIP);
    };

    const ULONGLONG elapsedMs = (GetTickCount64() >= m_prepareStartTick) ? (GetTickCount64() - m_prepareStartTick) : 0ULL;
    const float tSec = static_cast<float>(elapsedMs) / 1000.0f;
    const ComPtr<ID2D1StrokeStyle> roundSt = createRoundStrokeStyle(m_d2dFactory.Get());

    const float cardW = 456.0f;
    const float cardH = 264.0f;
    const float cx = W * 0.5f;
    const float cy = (46.0f + H) * 0.5f;
    const D2D1_RECT_F cardRc = D2D1::RectF(cx - cardW * 0.5f, cy - cardH * 0.5f, cx + cardW * 0.5f, cy + cardH * 0.5f);

    // 柔和环境呼吸晕影 + 高定微晶悬浮卡片
    const float pulse = 0.5f + 0.5f * std::sin(tSec * 3.2f);
    setBrush(D2D1::ColorF(accentRgb.r, accentRgb.g, accentRgb.b, (dark ? 0.12f : 0.07f) + 0.04f * pulse));
    m_renderTarget->FillRoundedRectangle(
        D2D1::RoundedRect(D2D1::RectF(cardRc.left - 8.0f, cardRc.top - 6.0f, cardRc.right + 8.0f, cardRc.bottom + 10.0f), 22.0f, 22.0f),
        brush);

    setBrush(D2D1::ColorF(0.02f, 0.04f, 0.10f, dark ? 0.42f : 0.09f));
    m_renderTarget->FillRoundedRectangle(
        D2D1::RoundedRect(D2D1::RectF(cardRc.left, cardRc.top + 4.0f, cardRc.right, cardRc.bottom + 6.0f), 16.0f, 16.0f),
        brush);
    m_renderTarget->FillRoundedRectangle(D2D1::RoundedRect(cardRc, 16.0f, 16.0f), brush);
    setBrush(colCardBg);
    m_renderTarget->FillRoundedRectangle(D2D1::RoundedRect(cardRc, 16.0f, 16.0f), brush);
    setBrush(colBorder);
    m_renderTarget->DrawRoundedRectangle(D2D1::RoundedRect(cardRc, 16.0f, 16.0f), brush, 1.0f);

    // 居中 FocuSee 风格双环旋转光学快门动效
    const float ringCy = cardRc.top + 68.0f;
    const float ringR = 26.0f;
    setBrush(D2D1::ColorF(accentRgb.r, accentRgb.g, accentRgb.b, dark ? 0.16f : 0.10f));
    m_renderTarget->FillEllipse(D2D1::Ellipse({cx, ringCy}, ringR + 7.0f + 2.5f * pulse, ringR + 7.0f + 2.5f * pulse), brush);
    setBrush(dark ? D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.10f) : D2D1::ColorF(0.08f, 0.14f, 0.26f, 0.10f));
    m_renderTarget->DrawEllipse(D2D1::Ellipse({cx, ringCy}, ringR, ringR), brush, 2.8f);

    if (m_d2dFactory) {
        const float startRad = tSec * 4.4f;
        const float sweepRad = 2.05f + 0.45f * std::sin(tSec * 2.6f);
        const D2D1_POINT_2F p0{cx + ringR * std::cos(startRad), ringCy + ringR * std::sin(startRad)};
        const D2D1_POINT_2F p1{cx + ringR * std::cos(startRad + sweepRad), ringCy + ringR * std::sin(startRad + sweepRad)};
        ComPtr<ID2D1PathGeometry> arcGeo;
        if (SUCCEEDED(m_d2dFactory->CreatePathGeometry(arcGeo.GetAddressOf())) && arcGeo) {
            ComPtr<ID2D1GeometrySink> sk;
            if (SUCCEEDED(arcGeo->Open(sk.GetAddressOf())) && sk) {
                sk->BeginFigure(p0, D2D1_FIGURE_BEGIN_HOLLOW);
                sk->AddArc(D2D1::ArcSegment(
                    p1, D2D1::SizeF(ringR, ringR), 0.0f,
                    D2D1_SWEEP_DIRECTION_CLOCKWISE,
                    sweepRad > 3.14159f ? D2D1_ARC_SIZE_LARGE : D2D1_ARC_SIZE_SMALL));
                sk->EndFigure(D2D1_FIGURE_END_OPEN);
                sk->Close();
                setBrush(colAccent);
                m_renderTarget->DrawGeometry(arcGeo.Get(), brush, 3.0f, roundSt.Get());
            }
        }
    }

    // 中心镜头微标
    setBrush(colAccent);
    m_renderTarget->FillEllipse(D2D1::Ellipse({cx, ringCy}, 5.2f, 5.2f), brush);
    setBrush(D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.95f));
    m_renderTarget->DrawEllipse(D2D1::Ellipse({cx, ringCy}, 9.5f, 9.5f), brush, 1.5f);

    // 主标题与三阶段实时进度说明
    drawTxt(isZh ? L"正在准备录屏演播室工程..." : L"Preparing Studio Workspace...",
            m_titleFmt.Get(), D2D1::RectF(cardRc.left + 24.0f, cardRc.top + 114.0f, cardRc.right - 24.0f, cardRc.top + 140.0f), colPrimary);

    const wchar_t* stageMsg = nullptr;
    if (elapsedMs < 750) {
        stageMsg = isZh ? L"正在高速封装超清录屏母带并校验音画同步索引..."
                        : L"Finalizing lossless master video and A/V sync index...";
    } else if (elapsedMs < 1900) {
        stageMsg = isZh ? L"正在构建声学山峦音轨、电影胶片缩略图与自动运镜曲线..."
                        : L"Building sculpted audio waveform, filmstrip & smart camera...";
    } else {
        stageMsg = isZh ? L"正在初始化视网膜超清预览画布，即将就绪..."
                        : L"Initializing Retina preview stage, almost ready...";
    }
    drawTxt(stageMsg, m_bodyFmt.Get(),
            D2D1::RectF(cardRc.left + 24.0f, cardRc.top + 144.0f, cardRc.right - 24.0f, cardRc.top + 168.0f), colSecondary);

    // 无极流光往返进度槽
    const D2D1_RECT_F barTrack = D2D1::RectF(cardRc.left + 44.0f, cardRc.top + 184.0f, cardRc.right - 44.0f, cardRc.top + 190.0f);
    setBrush(dark ? D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.09f) : D2D1::ColorF(0.08f, 0.14f, 0.26f, 0.08f));
    m_renderTarget->FillRoundedRectangle(D2D1::RoundedRect(barTrack, 3.0f, 3.0f), brush);

    const float barW = barTrack.right - barTrack.left;
    const float runnerW = barW * 0.36f;
    const float travel = 0.5f - 0.5f * std::cos(tSec * 2.8f);
    const float runX = barTrack.left + (barW - runnerW) * travel;
    setBrush(colAccent);
    m_renderTarget->FillRoundedRectangle(
        D2D1::RoundedRect(D2D1::RectF(runX, barTrack.top, runX + runnerW, barTrack.bottom), 3.0f, 3.0f), brush);

    // 底部已录时长微晶胶囊
    if (m_prepareRecordedDurSec > 0.05) {
        const std::wstring durBadge = formatCompactDurationBadge(m_prepareRecordedDurSec, isZh);
        const std::wstring footTxt = isZh
            ? (L"本次录制时长 " + durBadge + L" · 无损母带保护中")
            : (L"Recorded " + durBadge + L" · Lossless master protected");
        const D2D1_RECT_F pillRc = D2D1::RectF(cx - 132.0f, cardRc.top + 210.0f, cx + 132.0f, cardRc.top + 236.0f);
        setBrush(dark ? D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.055f) : D2D1::ColorF(0.06f, 0.10f, 0.18f, 0.048f));
        m_renderTarget->FillRoundedRectangle(D2D1::RoundedRect(pillRc, 6.5f, 6.5f), brush);
        drawTxt(footTxt, m_captionFmt.Get(), pillRc, colSecondary);
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
    m_coachHoverZones.clear();
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
        if (!fmt || txt.empty() || r.right <= r.left || r.bottom <= r.top) return;
        fmt->SetTextAlignment(hAlign);
        fmt->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
        setBrush(col);
        m_renderTarget->DrawText(txt.c_str(), static_cast<UINT32>(txt.size()), fmt, r, brush.Get(), D2D1_DRAW_TEXT_OPTIONS_CLIP);
    };

    const ComPtr<ID2D1StrokeStyle> roundStroke = createRoundStrokeStyle(m_d2dFactory.Get());

    // 1. 顶部标题栏：镜头品牌微标 + 规格微晶胶囊 + 次级白瓷按钮/立体渐变主按钮 + 原生最小化/最大化/关闭三键
    const float headerH = 46.0f;
    {
        const D2D1_RECT_F logoBadge = D2D1::RectF(14.0f, 11.0f, 38.0f, 35.0f);
        fillLinearVGradRounded(
            m_renderTarget.Get(), logoBadge, 6.5f,
            D2D1::ColorF((std::min)(1.0f, accentRgb.r + 0.08f), (std::min)(1.0f, accentRgb.g + 0.08f), (std::min)(1.0f, accentRgb.b + 0.08f), 1.0f),
            D2D1::ColorF(accentRgb.r * 0.84f, accentRgb.g * 0.84f, accentRgb.b * 0.92f, 1.0f));
        const float lcx = (logoBadge.left + logoBadge.right) * 0.5f;
        const float lcy = (logoBadge.top + logoBadge.bottom) * 0.5f;
        setBrush(D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.95f));
        m_renderTarget->DrawEllipse(D2D1::Ellipse({lcx, lcy}, 6.2f, 6.2f), brush.Get(), 1.5f);
        m_renderTarget->FillEllipse(D2D1::Ellipse({lcx, lcy}, 2.3f, 2.3f), brush.Get());
        setBrush(D2D1::ColorF(1.0f, 0.42f, 0.42f, 0.98f));
        m_renderTarget->FillEllipse(D2D1::Ellipse({logoBadge.right - 4.8f, logoBadge.top + 4.8f}, 1.9f, 1.9f), brush.Get());
    }

    const std::wstring headerTitle = isZh ? L"Tools3000 录屏演播室" : L"Tools3000 Studio";
    const float titleRight = isZh ? 236.0f : 196.0f;
    drawTextAligned(headerTitle, m_titleFmt.Get(), D2D1::RectF(46.0f, 0.0f, titleRight, headerH), colPrimary);

    if (m_preparingSession) {
        const D2D1_RECT_F closeRc = D2D1::RectF(W - 40.0f, 9.0f, W - 10.0f, 37.0f);
        const D2D1_RECT_F maxRc   = D2D1::RectF(closeRc.left - 33.0f, 9.0f, closeRc.left - 3.0f, 37.0f);
        const D2D1_RECT_F minRc   = D2D1::RectF(maxRc.left - 33.0f, 9.0f, maxRc.left - 3.0f, 37.0f);
        m_hitZones.push_back({Hit_MinimizeBtn, minRc, -1});
        m_hitZones.push_back({Hit_MaxRestoreBtn, maxRc, -1});

        setBrush(colSecondary);
        {
            const float mcx = (minRc.left + minRc.right) * 0.5f;
            const float mcy = (minRc.top + minRc.bottom) * 0.5f;
            m_renderTarget->DrawLine({mcx - 5.0f, mcy}, {mcx + 5.0f, mcy}, brush.Get(), 1.6f, roundStroke.Get());
        }
        {
            const float xcx = (maxRc.left + maxRc.right) * 0.5f;
            const float xcy = (maxRc.top + maxRc.bottom) * 0.5f;
            const D2D1_RECT_F maxBox = D2D1::RectF(xcx - 4.8f, xcy - 4.6f, xcx + 4.8f, xcy + 4.6f);
            m_renderTarget->DrawRoundedRectangle(D2D1::RoundedRect(maxBox, 1.6f, 1.6f), brush.Get(), 1.55f);
        }
        setBrush(D2D1::ColorF(colSecondary.r, colSecondary.g, colSecondary.b, 0.42f));
        const float closeCx = (closeRc.left + closeRc.right) * 0.5f;
        const float closeCy = (closeRc.top + closeRc.bottom) * 0.5f;
        m_renderTarget->DrawLine({closeCx - 4.8f, closeCy - 4.8f}, {closeCx + 4.8f, closeCy + 4.8f}, brush.Get(), 1.7f, roundStroke.Get());
        m_renderTarget->DrawLine({closeCx + 4.8f, closeCy - 4.8f}, {closeCx - 4.8f, closeCy + 4.8f}, brush.Get(), 1.7f, roundStroke.Get());

        setBrush(colBorder);
        m_renderTarget->DrawLine({0.0f, headerH}, {W, headerH}, brush.Get(), 1.0f);

        renderPreparingOverlay(W, H, dark, isZh, brush.Get());
        if (m_renderTarget->EndDraw() == D2DERR_RECREATE_TARGET) {
            discardDeviceResources();
        }
        return;
    }

    const double keptDurSec = computeEffectiveDurationSec();
    const std::wstring rawDurTxt = formatCompactDurationBadge(m_durationSec, isZh);
    const std::wstring keptDurTxt = formatCompactDurationBadge(keptDurSec, isZh);
    wchar_t metaBuf[112]{};
    if (std::abs(keptDurSec - m_durationSec) > 0.08) {
        std::swprintf(metaBuf, std::size(metaBuf),
                      L"%d×%d · %s -> %s",
                      m_origWidth, m_origHeight, rawDurTxt.c_str(), keptDurTxt.c_str());
    } else {
        std::swprintf(metaBuf, std::size(metaBuf),
                      L"%d×%d · %s",
                      m_origWidth, m_origHeight, rawDurTxt.c_str());
    }
    const std::wstring metaStr(metaBuf);
    const float metaPillW = std::clamp(static_cast<float>(metaStr.size()) * 7.4f + 20.0f, 96.0f, 256.0f);
    const D2D1_RECT_F metaPillRc = D2D1::RectF(titleRight + 4.0f, 12.0f, titleRight + 4.0f + metaPillW, 34.0f);
    if (metaPillRc.right < W - 340.0f) {
        setBrush(dark ? D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.055f) : D2D1::ColorF(0.06f, 0.10f, 0.18f, 0.048f));
        m_renderTarget->FillRoundedRectangle(D2D1::RoundedRect(metaPillRc, 6.0f, 6.0f), brush.Get());
        setBrush(colBorder);
        m_renderTarget->DrawRoundedRectangle(D2D1::RoundedRect(metaPillRc, 6.0f, 6.0f), brush.Get(), 0.85f);
        drawTextAligned(metaStr, m_monoFmt.Get(), metaPillRc, colSecondary, DWRITE_TEXT_ALIGNMENT_CENTER);
    }

    const bool exporting = m_exporting.load(std::memory_order_acquire);
    const D2D1_RECT_F closeRc = D2D1::RectF(W - 40.0f, 9.0f, W - 10.0f, 37.0f);
    const D2D1_RECT_F maxRc   = D2D1::RectF(closeRc.left - 33.0f, 9.0f, closeRc.left - 3.0f, 37.0f);
    const D2D1_RECT_F minRc   = D2D1::RectF(maxRc.left - 33.0f, 9.0f, maxRc.left - 3.0f, 37.0f);
    const float hdrSepX       = minRc.left - 9.0f;
    const D2D1_RECT_F exportBtnRc = D2D1::RectF(hdrSepX - 102.0f, 8.0f, hdrSepX - 10.0f, 38.0f);
    const D2D1_RECT_F keepBtnRc   = D2D1::RectF(exportBtnRc.left - 90.0f, 8.0f, exportBtnRc.left - 8.0f, 38.0f);

    if (!exporting && !m_exportModalOpen) {
        m_hitZones.push_back({Hit_KeepOriginalBtn, keepBtnRc, -1});
        m_hitZones.push_back({Hit_OpenExportModalBtn, exportBtnRc, -1});
        m_hitZones.push_back({Hit_MinimizeBtn, minRc, -1});
        m_hitZones.push_back({Hit_MaxRestoreBtn, maxRc, -1});
    }
    m_hitZones.push_back({Hit_CloseBtn, closeRc, -1});

    // 次级按钮：微晶白瓷/曜石立体键帽 [保留原片]
    {
        const bool hovKeep = (m_hoveredHitId == Hit_KeepOriginalBtn && !m_exportModalOpen);
        fillLinearVGradRounded(
            m_renderTarget.Get(), keepBtnRc, 7.5f,
            dark ? D2D1::ColorF(0.145f, 0.175f, 0.245f, hovKeep ? 1.0f : 0.88f)
                 : D2D1::ColorF(1.0f, 1.0f, 1.0f, 1.0f),
            dark ? D2D1::ColorF(0.110f, 0.135f, 0.195f, hovKeep ? 1.0f : 0.88f)
                 : D2D1::ColorF(0.952f, 0.965f, 0.985f, 1.0f));
        setBrush(hovKeep ? colAccent : colBorder);
        m_renderTarget->DrawRoundedRectangle(D2D1::RoundedRect(keepBtnRc, 7.5f, 7.5f), brush.Get(), hovKeep ? 1.15f : 0.95f);
        drawTextAligned(isZh ? L"保留原片" : L"Keep Raw", m_captionFmt.Get(), keepBtnRc,
                        hovKeep ? colPrimary : (dark ? colPrimary : D2D1::ColorF(0.16f, 0.22f, 0.32f, 1.0f)),
                        DWRITE_TEXT_ALIGNMENT_CENTER);
    }

    // 主按钮：双色微渐变 + 顶部镜面亮边 + 矢量导出图标 [导出]
    {
        const bool hovExp = (m_hoveredHitId == Hit_OpenExportModalBtn && !m_exportModalOpen);
        const float lift = hovExp ? 0.06f : 0.0f;
        setBrush(D2D1::ColorF(accentRgb.r * 0.4f, accentRgb.g * 0.4f, accentRgb.b * 0.5f, dark ? 0.36f : 0.20f));
        m_renderTarget->FillRoundedRectangle(
            D2D1::RoundedRect(D2D1::RectF(exportBtnRc.left, exportBtnRc.top + 1.5f, exportBtnRc.right, exportBtnRc.bottom + 1.8f), 7.5f, 7.5f),
            brush.Get());
        fillLinearVGradRounded(
            m_renderTarget.Get(), exportBtnRc, 7.5f,
            D2D1::ColorF((std::min)(1.0f, accentRgb.r + 0.08f + lift), (std::min)(1.0f, accentRgb.g + 0.08f + lift), (std::min)(1.0f, accentRgb.b + 0.08f + lift), 1.0f),
            D2D1::ColorF((std::max)(0.0f, accentRgb.r - 0.05f + lift), (std::max)(0.0f, accentRgb.g - 0.05f + lift), (std::max)(0.0f, accentRgb.b - 0.02f + lift), 1.0f));
        setBrush(D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.28f));
        m_renderTarget->DrawLine({exportBtnRc.left + 7.0f, exportBtnRc.top + 0.8f},
                                 {exportBtnRc.right - 7.0f, exportBtnRc.top + 0.8f}, brush.Get(), 0.9f);
        setBrush(D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.98f));
        drawCrispExportIcon(m_renderTarget.Get(), brush.Get(), exportBtnRc.left + 21.0f, (exportBtnRc.top + exportBtnRc.bottom) * 0.5f, roundStroke.Get());
        drawTextAligned(isZh ? L"导出" : L"Export", m_sectionFmt.Get(),
                        D2D1::RectF(exportBtnRc.left + 31.0f, exportBtnRc.top, exportBtnRc.right - 6.0f, exportBtnRc.bottom),
                        D2D1::ColorF(1.0f, 1.0f, 1.0f, 1.0f), DWRITE_TEXT_ALIGNMENT_CENTER);
    }

    // 顶栏竖向微分隔线
    setBrush(colBorder);
    m_renderTarget->DrawLine({hdrSepX, 13.0f}, {hdrSepX, 33.0f}, brush.Get(), 1.0f);

    // 窗口控制三键：[最小化] [最大化/还原] [关闭]
    if (m_hoveredHitId == Hit_MinimizeBtn && !m_exportModalOpen) {
        setBrush(colChipBg);
        m_renderTarget->FillRoundedRectangle(D2D1::RoundedRect(minRc, 6.5f, 6.5f), brush.Get());
    }
    setBrush((m_hoveredHitId == Hit_MinimizeBtn && !m_exportModalOpen) ? colPrimary : colSecondary);
    {
        const float mcx = (minRc.left + minRc.right) * 0.5f;
        const float mcy = (minRc.top + minRc.bottom) * 0.5f;
        m_renderTarget->DrawLine({mcx - 5.0f, mcy}, {mcx + 5.0f, mcy}, brush.Get(), 1.6f, roundStroke.Get());
    }

    if (m_hoveredHitId == Hit_MaxRestoreBtn && !m_exportModalOpen) {
        setBrush(colChipBg);
        m_renderTarget->FillRoundedRectangle(D2D1::RoundedRect(maxRc, 6.5f, 6.5f), brush.Get());
    }
    setBrush((m_hoveredHitId == Hit_MaxRestoreBtn && !m_exportModalOpen) ? colPrimary : colSecondary);
    {
        const float xcx = (maxRc.left + maxRc.right) * 0.5f;
        const float xcy = (maxRc.top + maxRc.bottom) * 0.5f;
        if (m_isMaximized) {
            const D2D1_RECT_F frontBox = D2D1::RectF(xcx - 4.8f, xcy - 2.8f, xcx + 2.8f, xcy + 4.8f);
            m_renderTarget->DrawRoundedRectangle(D2D1::RoundedRect(frontBox, 1.3f, 1.3f), brush.Get(), 1.45f);
            m_renderTarget->DrawLine({xcx - 2.2f, xcy - 4.8f}, {xcx + 4.8f, xcy - 4.8f}, brush.Get(), 1.35f, roundStroke.Get());
            m_renderTarget->DrawLine({xcx + 4.8f, xcy - 4.8f}, {xcx + 4.8f, xcy + 2.2f}, brush.Get(), 1.35f, roundStroke.Get());
        } else {
            const D2D1_RECT_F maxBox = D2D1::RectF(xcx - 4.8f, xcy - 4.6f, xcx + 4.8f, xcy + 4.6f);
            m_renderTarget->DrawRoundedRectangle(D2D1::RoundedRect(maxBox, 1.6f, 1.6f), brush.Get(), 1.55f);
        }
    }

    if (m_hoveredHitId == Hit_CloseBtn && !m_exportModalOpen) {
        setBrush(D2D1::ColorF(0.937f, 0.267f, 0.267f, 0.18f));
        m_renderTarget->FillRoundedRectangle(D2D1::RoundedRect(closeRc, 6.5f, 6.5f), brush.Get());
    }
    setBrush((m_hoveredHitId == Hit_CloseBtn && !m_exportModalOpen) ? D2D1::ColorF(0.937f, 0.267f, 0.267f, 1.0f) : colSecondary);
    const float closeCx = (closeRc.left + closeRc.right) * 0.5f;
    const float closeCy = (closeRc.top + closeRc.bottom) * 0.5f;
    m_renderTarget->DrawLine({closeCx - 4.8f, closeCy - 4.8f}, {closeCx + 4.8f, closeCy + 4.8f}, brush.Get(), 1.7f, roundStroke.Get());
    m_renderTarget->DrawLine({closeCx + 4.8f, closeCy - 4.8f}, {closeCx - 4.8f, closeCy + 4.8f}, brush.Get(), 1.7f, roundStroke.Get());

    setBrush(colBorder);
    m_renderTarget->DrawLine({0.0f, headerH}, {W, headerH}, brush.Get(), 1.0f);

    // 2. 左侧图标栏 + 响应式自适应抽屉面板（窗口放大时抽屉适度舒展，主空间优先赋予预览大舞台）
    const float contentTop = headerH + 10.0f;
    const float contentBottom = H - 12.0f;
    const D2D1_RECT_F railRc = D2D1::RectF(12.0f, contentTop, 60.0f, contentBottom);
    const float drawerW = std::clamp((W - 84.0f) * 0.235f, 286.0f, 336.0f);
    const D2D1_RECT_F drawerRc = D2D1::RectF(railRc.right + 8.0f, contentTop, railRc.right + 8.0f + drawerW, contentBottom);
    renderLeftRailAndDrawer(railRc, drawerRc, dark, isZh, brush.Get());

    // 3. 右侧宽敞预览大舞台
    const float rightAreaLeft = drawerRc.right + 10.0f;
    const float rightAreaRight = W - 12.0f;
    const float timelineCardH = 196.0f;
    const float stageBottom = contentBottom - timelineCardH - 8.0f;
    const D2D1_RECT_F stageRc = D2D1::RectF(rightAreaLeft, contentTop, rightAreaRight, stageBottom);

    setBrush(colStageBg);
    m_renderTarget->FillRoundedRectangle(D2D1::RoundedRect(stageRc, 12.0f, 12.0f), brush.Get());
    setBrush(colBorder);
    m_renderTarget->DrawRoundedRectangle(D2D1::RoundedRect(stageRc, 12.0f, 12.0f), brush.Get(), 1.0f);

    const D2D1_RECT_F videoViewport = D2D1::RectF(
        stageRc.left + 10.0f, stageRc.top + 10.0f, stageRc.right - 10.0f, stageRc.bottom - 10.0f);
    m_stageViewportPhysW = (std::max)(320, static_cast<int>(std::round((videoViewport.right - videoViewport.left) * m_dpiScale)));
    m_stageViewportPhysH = (std::max)(240, static_cast<int>(std::round((videoViewport.bottom - videoViewport.top) * m_dpiScale)));
    m_hitZones.push_back({Hit_StageCanvas, videoViewport, -1});
    m_lastWatermarkStageRect = D2D1::RectF(0.0f, 0.0f, 0.0f, 0.0f);

    if (m_frameBitmap && m_bitmapW > 0 && m_bitmapH > 0) {
        const float availW = (std::max)(32.0f, videoViewport.right - videoViewport.left);
        const float availH = (std::max)(32.0f, videoViewport.bottom - videoViewport.top);
        const float fit = (std::min)(availW / static_cast<float>(m_bitmapW), availH / static_cast<float>(m_bitmapH));
        const float drawW = std::round(static_cast<float>(m_bitmapW) * fit * m_dpiScale) / m_dpiScale;
        const float drawH = std::round(static_cast<float>(m_bitmapH) * fit * m_dpiScale) / m_dpiScale;
        const float dx = std::round((videoViewport.left + (availW - drawW) * 0.5f) * m_dpiScale) / m_dpiScale;
        const float dy = std::round((videoViewport.top + (availH - drawH) * 0.5f) * m_dpiScale) / m_dpiScale;
        m_lastBitmapDrawRect = D2D1::RectF(dx, dy, dx + drawW, dy + drawH);

        ComPtr<ID2D1DeviceContext> dc1;
        if (SUCCEEDED(m_renderTarget.As(&dc1)) && dc1) {
            dc1->DrawBitmap(
                m_frameBitmap.Get(),
                &m_lastBitmapDrawRect,
                1.0f,
                D2D1_INTERPOLATION_MODE_HIGH_QUALITY_CUBIC,
                nullptr,
                nullptr);
        } else {
            m_renderTarget->DrawBitmap(
                m_frameBitmap.Get(), m_lastBitmapDrawRect, 1.0f, D2D1_BITMAP_INTERPOLATION_MODE_LINEAR);
        }

        // 单点品牌图片/文字角标：计算舞台映射包围盒并在水印页或悬停/拖拽态显示微晶虚线框与右下角无级缩放手柄
        if (m_config.watermark.enabled && m_config.watermark.mode != WatermarkMode::TiledTraceability) {
            cv::Rect badgePx{};
            if (WatermarkRenderer::computeSingleBadgeBounds(
                    static_cast<int>(m_bitmapW), static_cast<int>(m_bitmapH), m_config.watermark, badgePx) &&
                badgePx.width > 0 && badgePx.height > 0) {
                const float sx0 = dx + (static_cast<float>(badgePx.x) / static_cast<float>(m_bitmapW)) * drawW;
                const float sy0 = dy + (static_cast<float>(badgePx.y) / static_cast<float>(m_bitmapH)) * drawH;
                const float sx1 = dx + (static_cast<float>(badgePx.x + badgePx.width) / static_cast<float>(m_bitmapW)) * drawW;
                const float sy1 = dy + (static_cast<float>(badgePx.y + badgePx.height) / static_cast<float>(m_bitmapH)) * drawH;
                m_lastWatermarkStageRect = D2D1::RectF(sx0, sy0, sx1, sy1);

                if (m_activeTab == LeftRailTab::Watermark || m_hoveredWatermarkOnStage || m_watermarkStageDragMode != 0) {
                    const D2D1_RECT_F wmGui = D2D1::RectF(sx0 - 4.0f, sy0 - 4.0f, sx1 + 4.0f, sy1 + 4.0f);
                    D2D1_STROKE_STYLE_PROPERTIES wsp = D2D1::StrokeStyleProperties();
                    wsp.dashStyle = D2D1_DASH_STYLE_DASH;
                    ComPtr<ID2D1StrokeStyle> wds;
                    m_d2dFactory->CreateStrokeStyle(wsp, nullptr, 0, wds.GetAddressOf());
                    setBrush(D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.85f));
                    m_renderTarget->DrawRoundedRectangle(D2D1::RoundedRect(wmGui, 5.0f, 5.0f), brush.Get(), 1.5f, wds.Get());
                    setBrush(colAccent);
                    m_renderTarget->DrawRoundedRectangle(D2D1::RoundedRect(wmGui, 5.0f, 5.0f), brush.Get(), 1.1f, wds.Get());

                    const float hr = (m_hoveredWatermarkCorner || m_watermarkStageDragMode == 2) ? 6.4f : 5.2f;
                    setBrush(colAccent);
                    m_renderTarget->FillEllipse(D2D1::Ellipse({wmGui.right, wmGui.bottom}, hr, hr), brush.Get());
                    setBrush(D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.98f));
                    m_renderTarget->DrawEllipse(D2D1::Ellipse({wmGui.right, wmGui.bottom}, hr, hr), brush.Get(), 1.5f);
                }
            }
        }

        if (m_selectedZoomIdx >= 0 && m_selectedZoomIdx < static_cast<int>(m_timeline.zoomSegments.size())) {
            const auto& zseg = m_timeline.zoomSegments[static_cast<size_t>(m_selectedZoomIdx)];
            const float srcW = static_cast<float>((std::max)(1, m_timeline.sourceWidth));
            const float srcH = static_cast<float>((std::max)(1, m_timeline.sourceHeight));
            const float normFx = std::clamp(zseg.focusX / srcW, 0.0f, 1.0f);
            const float normFy = std::clamp(zseg.focusY / srcH, 0.0f, 1.0f);
            float sx = 0.0f, sy = 0.0f;
            if (mapSourceNormToStagePoint(normFx, normFy, sx, sy) &&
                pointInRectF(m_lastBitmapDrawRect, sx, sy)) {
                if (m_draggingZoomFocus) {
                    // 100% 全景光学取景框模式 (Panoramic Optical Viewfinder)：0ms GPU 直绘放大视野框 + 四周柔和暗角 + 1:1 贴手准星
                    const float effZoom = std::clamp(
                        zseg.manualInserted ? zseg.targetZoom : m_config.defaultZoomFactor, 1.25f, 2.80f);
                    const float halfSpan = 0.5f / effZoom;
                    const float cropNormCx = std::clamp(normFx, halfSpan, 1.0f - halfSpan);
                    const float cropNormCy = std::clamp(normFy, halfSpan, 1.0f - halfSpan);
                    float vx0 = 0.0f, vy0 = 0.0f, vx1 = 0.0f, vy1 = 0.0f;
                    float sMinX = 0.0f, sMinY = 0.0f, sMaxX = 0.0f, sMaxY = 0.0f;
                    if (mapSourceNormToStagePoint(cropNormCx - halfSpan, cropNormCy - halfSpan, vx0, vy0) &&
                        mapSourceNormToStagePoint(cropNormCx + halfSpan, cropNormCy + halfSpan, vx1, vy1) &&
                        mapSourceNormToStagePoint(0.0f, 0.0f, sMinX, sMinY) &&
                        mapSourceNormToStagePoint(1.0f, 1.0f, sMaxX, sMaxY)) {
                        const D2D1_RECT_F fullSrcRc = D2D1::RectF(sMinX, sMinY, sMaxX, sMaxY);
                        const D2D1_RECT_F viewRc = D2D1::RectF(
                            (std::max)(fullSrcRc.left, vx0), (std::max)(fullSrcRc.top, vy0),
                            (std::min)(fullSrcRc.right, vx1), (std::min)(fullSrcRc.bottom, vy1));

                        // 四周光学取景暗角遮罩
                        setBrush(D2D1::ColorF(0.03f, 0.05f, 0.10f, 0.44f));
                        if (viewRc.top > fullSrcRc.top)
                            m_renderTarget->FillRectangle(D2D1::RectF(fullSrcRc.left, fullSrcRc.top, fullSrcRc.right, viewRc.top), brush.Get());
                        if (viewRc.bottom < fullSrcRc.bottom)
                            m_renderTarget->FillRectangle(D2D1::RectF(fullSrcRc.left, viewRc.bottom, fullSrcRc.right, fullSrcRc.bottom), brush.Get());
                        if (viewRc.left > fullSrcRc.left)
                            m_renderTarget->FillRectangle(D2D1::RectF(fullSrcRc.left, viewRc.top, viewRc.left, viewRc.bottom), brush.Get());
                        if (viewRc.right < fullSrcRc.right)
                            m_renderTarget->FillRectangle(D2D1::RectF(viewRc.right, viewRc.top, fullSrcRc.right, viewRc.bottom), brush.Get());

                        // 亮蓝视野外框 + 四角纯白徕卡 L 型光学对焦角标
                        setBrush(colAccent);
                        m_renderTarget->DrawRoundedRectangle(D2D1::RoundedRect(viewRc, 4.0f, 4.0f), brush.Get(), 1.6f);
                        const float cLen = (std::min)(14.0f, (std::min)(viewRc.right - viewRc.left, viewRc.bottom - viewRc.top) * 0.22f);
                        setBrush(D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.96f));
                        m_renderTarget->DrawLine({viewRc.left, viewRc.top + cLen}, {viewRc.left, viewRc.top}, brush.Get(), 2.4f, roundStroke.Get());
                        m_renderTarget->DrawLine({viewRc.left, viewRc.top}, {viewRc.left + cLen, viewRc.top}, brush.Get(), 2.4f, roundStroke.Get());
                        m_renderTarget->DrawLine({viewRc.right - cLen, viewRc.top}, {viewRc.right, viewRc.top}, brush.Get(), 2.4f, roundStroke.Get());
                        m_renderTarget->DrawLine({viewRc.right, viewRc.top}, {viewRc.right, viewRc.top + cLen}, brush.Get(), 2.4f, roundStroke.Get());
                        m_renderTarget->DrawLine({viewRc.left, viewRc.bottom - cLen}, {viewRc.left, viewRc.bottom}, brush.Get(), 2.4f, roundStroke.Get());
                        m_renderTarget->DrawLine({viewRc.left, viewRc.bottom}, {viewRc.left + cLen, viewRc.bottom}, brush.Get(), 2.4f, roundStroke.Get());
                        m_renderTarget->DrawLine({viewRc.right - cLen, viewRc.bottom}, {viewRc.right, viewRc.bottom}, brush.Get(), 2.4f, roundStroke.Get());
                        m_renderTarget->DrawLine({viewRc.right, viewRc.bottom - cLen}, {viewRc.right, viewRc.bottom}, brush.Get(), 2.4f, roundStroke.Get());

                        wchar_t vfBadge[48]{};
                        std::swprintf(vfBadge, std::size(vfBadge),
                                      isZh ? L"运镜特写视野 %.1fx" : L"Zoom Frame %.1fx", effZoom);
                        const float pillY = (viewRc.top - 24.0f >= fullSrcRc.top) ? (viewRc.top - 22.0f) : (viewRc.top + 6.0f);
                        const D2D1_RECT_F vfPill = D2D1::RectF(viewRc.left + 4.0f, pillY, (std::min)(viewRc.right - 4.0f, viewRc.left + 124.0f), pillY + 18.5f);
                        if (vfPill.right > vfPill.left + 48.0f) {
                            setBrush(D2D1::ColorF(0.06f, 0.10f, 0.18f, 0.84f));
                            m_renderTarget->FillRoundedRectangle(D2D1::RoundedRect(vfPill, 4.5f, 4.5f), brush.Get());
                            drawTextAligned(vfBadge, m_captionFmt.Get(), vfPill, D2D1::ColorF(1, 1, 1, 0.96f), DWRITE_TEXT_ALIGNMENT_CENTER);
                        }
                    }
                }

                const float retR = (m_draggingZoomFocus || m_hoveredZoomFocusReticle) ? 10.5f : 9.2f;
                setBrush(D2D1::ColorF(0.02f, 0.04f, 0.10f, 0.35f));
                m_renderTarget->DrawEllipse(D2D1::Ellipse({sx, sy + 0.8f}, retR + 1.0f, retR + 1.0f), brush.Get(), 2.4f);
                setBrush(D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.96f));
                m_renderTarget->DrawEllipse(D2D1::Ellipse({sx, sy}, retR, retR), brush.Get(), 2.2f);
                setBrush(colAccent);
                m_renderTarget->DrawEllipse(D2D1::Ellipse({sx, sy}, retR, retR), brush.Get(), 1.4f);
                m_renderTarget->FillEllipse(D2D1::Ellipse({sx, sy}, 2.2f, 2.2f), brush.Get());
                m_renderTarget->DrawLine({sx - 15.0f, sy}, {sx - 4.5f, sy}, brush.Get(), 1.35f, roundStroke.Get());
                m_renderTarget->DrawLine({sx + 4.5f, sy}, {sx + 15.0f, sy}, brush.Get(), 1.35f, roundStroke.Get());
                m_renderTarget->DrawLine({sx, sy - 15.0f}, {sx, sy - 4.5f}, brush.Get(), 1.35f, roundStroke.Get());
                m_renderTarget->DrawLine({sx, sy + 4.5f}, {sx, sy + 15.0f}, brush.Get(), 1.35f, roundStroke.Get());
            }
        }

        // 舞台悬停未选中可见标注时的轻柔虚线感应框（直观提示可直接抓起四向拖动）
        if (m_hoveredStageAnnIdx >= 0 &&
            m_hoveredStageAnnIdx != m_selectedAnnIdx &&
            m_hoveredStageAnnIdx < static_cast<int>(m_timeline.annotations.size())) {
            const auto& hovAnn = m_timeline.annotations[static_cast<size_t>(m_hoveredStageAnnIdx)];
            float hp0x = 0.0f, hp0y = 0.0f, hp1x = 0.0f, hp1y = 0.0f;
            if (mapSourceNormToStagePoint(hovAnn.x0, hovAnn.y0, hp0x, hp0y) &&
                mapSourceNormToStagePoint(hovAnn.x1, hovAnn.y1, hp1x, hp1y)) {
                D2D1_STROKE_STYLE_PROPERTIES hsp = D2D1::StrokeStyleProperties();
                hsp.dashStyle = D2D1_DASH_STYLE_DASH;
                ComPtr<ID2D1StrokeStyle> hds;
                m_d2dFactory->CreateStrokeStyle(hsp, nullptr, 0, hds.GetAddressOf());
                setBrush(D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.72f));
                if (isPointDirectedAnnotation(hovAnn.kind)) {
                    m_renderTarget->DrawEllipse(D2D1::Ellipse({hp0x, hp0y}, 4.5f, 4.5f), brush.Get(), 1.2f);
                    m_renderTarget->DrawEllipse(D2D1::Ellipse({hp1x, hp1y}, 5.5f, 5.5f), brush.Get(), 1.3f);
                } else {
                    const D2D1_RECT_F hbox = D2D1::RectF(
                        (std::min)(hp0x, hp1x) - 4.0f, (std::min)(hp0y, hp1y) - 4.0f,
                        (std::max)(hp0x, hp1x) + 4.0f, (std::max)(hp0y, hp1y) + 4.0f);
                    m_renderTarget->DrawRoundedRectangle(D2D1::RoundedRect(hbox, 4.0f, 4.0f), brush.Get(), 1.2f, hds.Get());
                }
            }
        }

        if (m_selectedAnnIdx >= 0 && m_selectedAnnIdx < static_cast<int>(m_timeline.annotations.size())) {
            const auto& ann = m_timeline.annotations[static_cast<size_t>(m_selectedAnnIdx)];
            float p0x = 0.0f, p0y = 0.0f, p1x = 0.0f, p1y = 0.0f;
            if (mapSourceNormToStagePoint(ann.x0, ann.y0, p0x, p0y) &&
                mapSourceNormToStagePoint(ann.x1, ann.y1, p1x, p1y)) {
                const bool hovStart = (m_hoveredStageAnnIdx == m_selectedAnnIdx && m_hoveredStageAnnPart == StageAnnDragKind::DragStartPoint);
                const bool hovEnd = (m_hoveredStageAnnIdx == m_selectedAnnIdx && m_hoveredStageAnnPart == StageAnnDragKind::DragEndPoint);
                setBrush(colAccent);
                if (isPointDirectedAnnotation(ann.kind)) {
                    const float r0 = hovStart ? 6.2f : 5.0f;
                    const float r1 = hovEnd ? 7.2f : 6.0f;
                    m_renderTarget->FillEllipse(D2D1::Ellipse({p0x, p0y}, r0, r0), brush.Get());
                    m_renderTarget->FillEllipse(D2D1::Ellipse({p1x, p1y}, r1, r1), brush.Get());
                    setBrush(D2D1::ColorF(1, 1, 1, 0.96f));
                    m_renderTarget->DrawEllipse(D2D1::Ellipse({p0x, p0y}, r0, r0), brush.Get(), 1.5f);
                    m_renderTarget->DrawEllipse(D2D1::Ellipse({p1x, p1y}, r1, r1), brush.Get(), 1.6f);
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
                        const float r0 = hovStart ? 6.2f : 5.0f;
                        const float r1 = hovEnd ? 6.6f : 5.5f;
                        m_renderTarget->FillEllipse(D2D1::Ellipse({boxRc.left, boxRc.top}, r0, r0), brush.Get());
                        m_renderTarget->FillEllipse(D2D1::Ellipse({boxRc.right, boxRc.bottom}, r1, r1), brush.Get());
                        setBrush(D2D1::ColorF(1, 1, 1, 0.95f));
                        m_renderTarget->DrawEllipse(D2D1::Ellipse({boxRc.left, boxRc.top}, r0, r0), brush.Get(), 1.4f);
                        m_renderTarget->DrawEllipse(D2D1::Ellipse({boxRc.right, boxRc.bottom}, r1, r1), brush.Get(), 1.4f);
                    }
                }
            }
        }
    }

    // 4. 右下角磁性波纹切片与三轨非线性时间轴卡片
    const D2D1_RECT_F tlCard = D2D1::RectF(rightAreaLeft, stageBottom + 8.0f, rightAreaRight, contentBottom);
    m_timelineCardRect = tlCard;
    clampTimelineViewWindow(keptDurSec);
    setBrush(colCardBg);
    m_renderTarget->FillRoundedRectangle(D2D1::RoundedRect(tlCard, 12.0f, 12.0f), brush.Get());
    setBrush(colBorder);
    m_renderTarget->DrawRoundedRectangle(D2D1::RoundedRect(tlCard, 12.0f, 12.0f), brush.Get(), 1.0f);

    // 世界级微晶陶瓷/曜石一体式工具控制舱（彻底告别彩色糖果塑料色块，采用沉降凹槽 + 白瓷键帽 + 圆头线性矢量图标）
    auto drawToolbarPod = [&](float podL, float podR) {
        const D2D1_RECT_F podRc = D2D1::RectF(podL, tlCard.top + 6.0f, podR, tlCard.top + 34.0f);
        setBrush(dark ? D2D1::ColorF(0.048f, 0.060f, 0.094f, 0.96f)
                      : D2D1::ColorF(0.925f, 0.940f, 0.965f, 1.0f));
        m_renderTarget->FillRoundedRectangle(D2D1::RoundedRect(podRc, 7.5f, 7.5f), brush.Get());
        setBrush(colBorder);
        m_renderTarget->DrawRoundedRectangle(D2D1::RoundedRect(podRc, 7.5f, 7.5f), brush.Get(), 0.85f);
    };

    auto drawPodButton = [&](int hitId, const D2D1_RECT_F& rc, const std::wstring& label,
                             int iconKind, bool enabled = true, bool activeHighlight = false,
                             bool dangerOnHover = false, bool useMonoLabel = false) {
        m_coachHoverZones.push_back({hitId, rc, enabled});
        if (enabled) {
            m_hitZones.push_back({hitId, rc, -1});
        }
        const bool hov = (enabled && m_hoveredHitId == hitId && !m_exportModalOpen);
        const bool coachHov = (!enabled && m_hoveredCoachId == hitId && !m_exportModalOpen);

        if (enabled && (hov || activeHighlight)) {
            setBrush(D2D1::ColorF(0.02f, 0.04f, 0.10f, dark ? 0.36f : 0.08f));
            m_renderTarget->FillRoundedRectangle(
                D2D1::RoundedRect(D2D1::RectF(rc.left, rc.top + 1.0f, rc.right, rc.bottom + 1.2f), 5.6f, 5.6f),
                brush.Get());
            fillLinearVGradRounded(
                m_renderTarget.Get(), rc, 5.6f,
                dark ? D2D1::ColorF(0.175f, 0.215f, 0.310f, 1.0f) : D2D1::ColorF(1.0f, 1.0f, 1.0f, 1.0f),
                dark ? D2D1::ColorF(0.138f, 0.172f, 0.254f, 1.0f) : D2D1::ColorF(0.976f, 0.985f, 1.0f, 1.0f));
            setBrush(hov && dangerOnHover
                ? D2D1::ColorF(0.92f, 0.30f, 0.34f, 0.48f)
                : (activeHighlight ? colAccent : (dark ? D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.15f) : D2D1::ColorF(0.06f, 0.10f, 0.20f, 0.09f))));
            m_renderTarget->DrawRoundedRectangle(D2D1::RoundedRect(rc, 5.6f, 5.6f), brush.Get(), 0.9f);
        } else if (coachHov) {
            setBrush(dark ? D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.04f) : D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.45f));
            m_renderTarget->FillRoundedRectangle(D2D1::RoundedRect(rc, 5.6f, 5.6f), brush.Get());
        }

        const float alphaMul = enabled ? 1.0f : (coachHov ? 0.48f : 0.32f);
        D2D1_COLOR_F iconCol = dark
            ? D2D1::ColorF(0.84f, 0.89f, 0.96f, alphaMul)
            : D2D1::ColorF(0.20f, 0.26f, 0.36f, alphaMul);
        D2D1_COLOR_F txtCol = dark
            ? D2D1::ColorF(0.94f, 0.96f, 0.99f, alphaMul)
            : D2D1::ColorF(0.10f, 0.15f, 0.24f, alphaMul);

        if (enabled && hov && dangerOnHover) {
            iconCol = D2D1::ColorF(0.92f, 0.26f, 0.30f, 1.0f);
            txtCol  = D2D1::ColorF(0.88f, 0.22f, 0.26f, 1.0f);
        } else if (enabled && (hov || activeHighlight)) {
            iconCol = colAccent;
            if (activeHighlight && !dark) txtCol = colAccent;
        }

        const float icx = rc.left + 13.5f;
        const float icy = (rc.top + rc.bottom) * 0.5f;
        setBrush(iconCol);
        switch (iconKind) {
            case 0: drawScissorsIcon(m_renderTarget.Get(), brush.Get(), icx, icy, roundStroke.Get()); break;
            case 1: drawCrispTrashIcon(m_renderTarget.Get(), brush.Get(), icx, icy, roundStroke.Get()); break;
            case 2: drawCrispSpeedIcon(m_renderTarget.Get(), brush.Get(), icx, icy, roundStroke.Get()); break;
            case 3: drawCrispUndoRedoIcon(m_d2dFactory.Get(), m_renderTarget.Get(), brush.Get(), icx, icy, true, roundStroke.Get()); break;
            case 4: drawCrispUndoRedoIcon(m_d2dFactory.Get(), m_renderTarget.Get(), brush.Get(), icx, icy, false, roundStroke.Get()); break;
            case 5: drawCrispPenIcon(m_d2dFactory.Get(), m_renderTarget.Get(), brush.Get(), icx, icy, roundStroke.Get()); break;
            case 6: drawCrispCameraFocusIcon(m_renderTarget.Get(), brush.Get(), icx, icy, roundStroke.Get()); break;
            default: break;
        }

        drawTextAligned(label, useMonoLabel ? m_monoFmt.Get() : m_captionFmt.Get(),
                        D2D1::RectF(rc.left + 22.5f, rc.top, rc.right - 4.0f, rc.bottom),
                        txtCol, DWRITE_TEXT_ALIGNMENT_CENTER);
    };

    auto drawPodDivider = [&](float xPos) {
        setBrush(dark ? D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.08f) : D2D1::ColorF(0.08f, 0.12f, 0.22f, 0.09f));
        m_renderTarget->DrawLine({xPos, tlCard.top + 12.0f}, {xPos, tlCard.top + 28.0f}, brush.Get(), 0.9f);
    };

    // 底部 3 组微晶分段控制舱：剪辑舱 [拆分(S) | 删除 | 1.0x] · 历史舱 [撤销 | 重做] · 创作舱 [标注 | 运镜]
    const float btnTop = tlCard.top + 8.2f;
    const float btnBot = tlCard.top + 31.8f;
    float curX = tlCard.left + 10.0f;

    // 舱 1：剪辑与变速舱
    {
        const float pod1L = curX;
        const D2D1_RECT_F splitBtnRc = D2D1::RectF(pod1L + 2.5f, btnTop, pod1L + 76.5f, btnBot);
        const D2D1_RECT_F delBtnRc   = D2D1::RectF(splitBtnRc.right + 3.0f, btnTop, splitBtnRc.right + 61.0f, btnBot);
        const D2D1_RECT_F spdRc      = D2D1::RectF(delBtnRc.right + 3.0f, btnTop, delBtnRc.right + 61.0f, btnBot);
        const float pod1R = spdRc.right + 2.5f;
        drawToolbarPod(pod1L, pod1R);
        drawPodDivider(splitBtnRc.right + 1.5f);
        drawPodDivider(delBtnRc.right + 1.5f);

        const bool canDeleteClip = (m_selectedClipIdx >= 0 && m_videoClips.size() > 1);
        const bool canDeleteAny  = canDeleteClip || m_selectedZoomIdx >= 0 || m_selectedAnnIdx >= 0;
        float curClipSpd = 1.0f;
        if (m_selectedClipIdx >= 0 && m_selectedClipIdx < static_cast<int>(m_videoClips.size())) {
            curClipSpd = m_videoClips[static_cast<size_t>(m_selectedClipIdx)].speed;
        }
        wchar_t spdLabel[24]{};
        std::swprintf(spdLabel, std::size(spdLabel), L"%.1fx", curClipSpd);

        drawPodButton(Hit_SplitAtPlayhead, splitBtnRc, isZh ? L"拆分(S)" : L"Split(S)", 0, true, false, false, false);
        drawPodButton(Hit_DeleteSelectedItem, delBtnRc, isZh ? L"删除" : L"Delete", 1, canDeleteAny, false, true, false);
        drawPodButton(Hit_CycleClipSpeed, spdRc, spdLabel, 2, !m_videoClips.empty(), std::abs(curClipSpd - 1.0f) > 0.03f, false, true);
        curX = pod1R + 7.0f;
    }

    // 舱 2：无损历史回溯舱
    {
        const float pod2L = curX;
        const D2D1_RECT_F undoRc = D2D1::RectF(pod2L + 2.5f, btnTop, pod2L + 58.5f, btnBot);
        const D2D1_RECT_F redoRc = D2D1::RectF(undoRc.right + 3.0f, btnTop, undoRc.right + 59.0f, btnBot);
        const float pod2R = redoRc.right + 2.5f;
        drawToolbarPod(pod2L, pod2R);
        drawPodDivider(undoRc.right + 1.5f);

        drawPodButton(Hit_UndoBtn, undoRc, isZh ? L"撤销" : L"Undo", 3, !m_undoStack.empty());
        drawPodButton(Hit_RedoBtn, redoRc, isZh ? L"重做" : L"Redo", 4, !m_redoStack.empty());
        curX = pod2R + 7.0f;
    }

    // 舱 3：视觉创作舱
    {
        const float pod3L = curX;
        const D2D1_RECT_F markQuickRc = D2D1::RectF(pod3L + 2.5f, btnTop, pod3L + 58.5f, btnBot);
        const D2D1_RECT_F addZoomRc   = D2D1::RectF(markQuickRc.right + 3.0f, btnTop, markQuickRc.right + 61.0f, btnBot);
        const float pod3R = addZoomRc.right + 2.5f;
        drawToolbarPod(pod3L, pod3R);
        drawPodDivider(markQuickRc.right + 1.5f);

        drawPodButton(Hit_QuickOpenAnnotations, markQuickRc, isZh ? L"标注" : L"Mark", 5, true, m_activeTab == LeftRailTab::Annotations);
        drawPodButton(Hit_AddZoomAtPlayhead, addZoomRc, isZh ? L"运镜" : L"Zoom", 6, true, false);
        curX = pod3R + 8.0f;
    }

    // ── 右翼：时间轴视野横向缩放舱 [ 全览/X.Xx | - ──o── + ]（替代原先重复的右上角播放小蓝条，支撑 1x~24x 局部精修） ──
    const float tlCardW = tlCard.right - tlCard.left;
    const bool wideZoomPod = (tlCardW >= 820.0f);
    const float zoomPodRight = tlCard.right - (wideZoomPod ? 12.0f : 10.0f);
    const float zoomPodLeft  = zoomPodRight - (wideZoomPod ? 156.0f : 90.0f);
    m_timelineTrackRect = D2D1::RectF(0.0f, 0.0f, 0.0f, 0.0f);
    {
        drawToolbarPod(zoomPodLeft, zoomPodRight);

        auto drawZoomKeyCap = [&](int hId, const D2D1_RECT_F& r, bool act = false) -> D2D1_COLOR_F {
            m_hitZones.push_back({hId, r, -1});
            const bool hov = (!m_exportModalOpen && m_hoveredHitId == hId);
            if (hov || act) {
                fillLinearVGradRounded(
                    m_renderTarget.Get(), r, 5.2f,
                    dark ? D2D1::ColorF(0.175f, 0.215f, 0.310f, 1.0f) : D2D1::ColorF(1.0f, 1.0f, 1.0f, 1.0f),
                    dark ? D2D1::ColorF(0.138f, 0.172f, 0.254f, 1.0f) : D2D1::ColorF(0.976f, 0.985f, 1.0f, 1.0f));
                setBrush(act ? colAccent : (dark ? D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.15f) : D2D1::ColorF(0.06f, 0.10f, 0.20f, 0.09f)));
                m_renderTarget->DrawRoundedRectangle(D2D1::RoundedRect(r, 5.2f, 5.2f), brush.Get(), 0.85f);
                return colAccent;
            }
            return colSecondary;
        };

        const bool zoomedIn = (m_timelineZoom > 1.02f);
        wchar_t fitLbl[24]{};
        if (zoomedIn) {
            std::swprintf(fitLbl, std::size(fitLbl), L"%.1fx", m_timelineZoom);
        } else {
            std::swprintf(fitLbl, std::size(fitLbl), isZh ? L"全览" : L"Fit");
        }

        if (wideZoomPod) {
            const D2D1_RECT_F fitRc  = D2D1::RectF(zoomPodLeft + 2.5f, btnTop, zoomPodLeft + 46.5f, btnBot);
            const D2D1_RECT_F zOutRc = D2D1::RectF(fitRc.right + 3.0f, btnTop, fitRc.right + 23.0f, btnBot);
            const D2D1_RECT_F zInRc  = D2D1::RectF(zoomPodRight - 22.5f, btnTop, zoomPodRight - 2.5f, btnBot);
            drawPodDivider(fitRc.right + 1.5f);

            const D2D1_COLOR_F fitCol = drawZoomKeyCap(Hit_TimelineZoomFit, fitRc, zoomedIn);
            drawTextAligned(fitLbl, zoomedIn ? m_monoFmt.Get() : m_captionFmt.Get(), fitRc,
                            zoomedIn ? colAccent : (m_hoveredHitId == Hit_TimelineZoomFit ? colPrimary : fitCol),
                            DWRITE_TEXT_ALIGNMENT_CENTER);

            const D2D1_COLOR_F outCol = drawZoomKeyCap(Hit_TimelineZoomOut, zOutRc);
            const float outCx = (zOutRc.left + zOutRc.right) * 0.5f;
            const float outCy = (zOutRc.top + zOutRc.bottom) * 0.5f;
            setBrush(outCol);
            m_renderTarget->DrawLine({outCx - 3.6f, outCy}, {outCx + 3.6f, outCy}, brush.Get(), 1.45f, roundStroke.Get());

            const D2D1_RECT_F zSliderTrk = D2D1::RectF(zOutRc.right + 8.5f, tlCard.top + 18.7f, zInRc.left - 8.5f, tlCard.top + 21.3f);
            m_sliderTrackRects[static_cast<size_t>(SliderDragKind::TimelineZoom)] = zSliderTrk;
            m_hitZones.push_back({Hit_TimelineZoomSlider, D2D1::RectF(zSliderTrk.left - 4.0f, btnTop, zSliderTrk.right + 4.0f, btnBot), -1});

            setBrush(dark ? D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.12f) : D2D1::ColorF(0.08f, 0.14f, 0.24f, 0.11f));
            m_renderTarget->FillRoundedRectangle(D2D1::RoundedRect(zSliderTrk, 1.3f, 1.3f), brush.Get());

            const float zNorm = std::clamp(std::log(m_timelineZoom) / std::log(24.0f), 0.0f, 1.0f);
            const float zKnobX = zSliderTrk.left + (zSliderTrk.right - zSliderTrk.left) * zNorm;
            if (zNorm > 0.01f) {
                setBrush(colAccent);
                m_renderTarget->FillRoundedRectangle(
                    D2D1::RoundedRect(D2D1::RectF(zSliderTrk.left, zSliderTrk.top, zKnobX, zSliderTrk.bottom), 1.3f, 1.3f),
                    brush.Get());
            }
            const float zCy = (zSliderTrk.top + zSliderTrk.bottom) * 0.5f;
            setBrush(D2D1::ColorF(0.02f, 0.04f, 0.10f, 0.20f));
            m_renderTarget->FillEllipse(D2D1::Ellipse({zKnobX, zCy + 0.8f}, 4.8f, 4.8f), brush.Get());
            setBrush(D2D1::ColorF(1.0f, 1.0f, 1.0f, 1.0f));
            m_renderTarget->FillEllipse(D2D1::Ellipse({zKnobX, zCy}, 4.5f, 4.5f), brush.Get());
            setBrush(colAccent);
            m_renderTarget->DrawEllipse(D2D1::Ellipse({zKnobX, zCy}, 4.5f, 4.5f), brush.Get(), 1.3f);

            const D2D1_COLOR_F inCol = drawZoomKeyCap(Hit_TimelineZoomIn, zInRc);
            const float inCx = (zInRc.left + zInRc.right) * 0.5f;
            const float inCy = (zInRc.top + zInRc.bottom) * 0.5f;
            setBrush(inCol);
            m_renderTarget->DrawLine({inCx - 3.6f, inCy}, {inCx + 3.6f, inCy}, brush.Get(), 1.45f, roundStroke.Get());
            m_renderTarget->DrawLine({inCx, inCy - 3.6f}, {inCx, inCy + 3.6f}, brush.Get(), 1.45f, roundStroke.Get());
        } else {
            const D2D1_RECT_F zOutRc = D2D1::RectF(zoomPodLeft + 2.5f, btnTop, zoomPodLeft + 22.5f, btnBot);
            const D2D1_RECT_F fitRc  = D2D1::RectF(zOutRc.right + 2.0f, btnTop, zoomPodRight - 24.5f, btnBot);
            const D2D1_RECT_F zInRc  = D2D1::RectF(zoomPodRight - 22.5f, btnTop, zoomPodRight - 2.5f, btnBot);
            const D2D1_COLOR_F outCol = drawZoomKeyCap(Hit_TimelineZoomOut, zOutRc);
            const float cy = (btnTop + btnBot) * 0.5f;
            setBrush(outCol);
            m_renderTarget->DrawLine({(zOutRc.left + zOutRc.right) * 0.5f - 3.4f, cy},
                                     {(zOutRc.left + zOutRc.right) * 0.5f + 3.4f, cy}, brush.Get(), 1.45f, roundStroke.Get());
            const D2D1_COLOR_F fitCol = drawZoomKeyCap(Hit_TimelineZoomFit, fitRc, zoomedIn);
            drawTextAligned(fitLbl, zoomedIn ? m_monoFmt.Get() : m_captionFmt.Get(), fitRc, fitCol, DWRITE_TEXT_ALIGNMENT_CENTER);
            const D2D1_COLOR_F inCol = drawZoomKeyCap(Hit_TimelineZoomIn, zInRc);
            const float inCx = (zInRc.left + zInRc.right) * 0.5f;
            setBrush(inCol);
            m_renderTarget->DrawLine({inCx - 3.4f, cy}, {inCx + 3.4f, cy}, brush.Get(), 1.45f, roundStroke.Get());
            m_renderTarget->DrawLine({inCx, cy - 3.4f}, {inCx, cy + 3.4f}, brush.Get(), 1.45f, roundStroke.Get());
        }
    }

    // ── 中枢：居中锚定的一体化微晶走带控制台 [ |<  <  [> / ||]  >  |  00:01.2 / 00:02.8 ] ──
    const double shownKeptSec = m_hoverSkimming ? m_hoverSkimSec : m_playheadSec;
    {
        const float idealCenter = (curX + zoomPodLeft) * 0.5f;
        const auto trans = renderIntegratedTransportConsole(
            m_d2dFactory.Get(), m_renderTarget.Get(), brush.Get(), m_monoFmt.Get(),
            idealCenter, curX + 2.0f, zoomPodLeft - 8.0f, tlCard.top + 6.0f,
            shownKeptSec, keptDurSec, m_playing,
            m_exportModalOpen ? 0 : m_hoveredHitId,
            dark, colAccent, colPrimary, colSecondary, colBorder, roundStroke.Get());
        if (!m_exportModalOpen) {
            m_hitZones.push_back({Hit_TransportJumpStart, trans.jumpStartBtn, -1});
            if (trans.hasFrameStepButtons) {
                m_hitZones.push_back({Hit_TransportPrevFrame, trans.prevFrameBtn, -1});
                m_hitZones.push_back({Hit_TransportNextFrame, trans.nextFrameBtn, -1});
            }
            m_hitZones.push_back({Hit_PlayPauseBtn, trans.playPauseBtn, -1});
        }
    }

    // 顶栏工具走带区与下方时间轴标尺的 1px 贯通柔和分隔丝线
    setBrush(dark ? D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.075f) : D2D1::ColorF(0.06f, 0.10f, 0.20f, 0.075f));
    m_renderTarget->DrawLine({tlCard.left + 10.0f, tlCard.top + 38.0f},
                             {tlCard.right - 10.0f, tlCard.top + 38.0f}, brush.Get(), 0.85f);

    const float laneLeft = tlCard.left + 80.0f;
    const float laneRight = tlCard.right - 14.0f;
    const float trackW = (std::max)(1.0f, laneRight - laneLeft);
    const double visSpanSec = visibleTimelineSpanSec(keptDurSec);

    // 涵盖整片轨道的悬停实时掠览区 (三级游标卡尺刻度 + 胶片山峦音视频轨 + 运镜轨 + 标注轨)
    m_allLanesHoverRect = D2D1::RectF(laneLeft, tlCard.top + 39.0f, laneRight, tlCard.top + 184.5f);
    m_hitZones.push_back({Hit_TimelineRulerScrub, D2D1::RectF(laneLeft, tlCard.top + 39.0f, laneRight, tlCard.top + 59.5f), -1});

    // 瑞士腕表 / Final Cut Pro 级三级长·中·短精密游标卡尺时间刻度尺
    renderBroadcastVernierTimeRuler(
        m_renderTarget.Get(), brush.Get(), m_monoFmt.Get(),
        laneLeft, laneRight, tlCard.top + 39.0f, tlCard.top + 58.8f,
        keptDurSec, m_timelineViewStartSec, visSpanSec, dark, colSecondary);

    // 轨道 1: Final Cut Pro / Screen Studio 双层联体「电影胶片画廊 + 雕刻声学山峦曲线」主轨 (60.5..124.5, 64px)
    m_cutLaneRect = D2D1::RectF(laneLeft, tlCard.top + 60.5f, laneRight, tlCard.top + 124.5f);
    {
        // 左侧精致声波微图标 + 轨道标题
        const float icx = tlCard.left + 21.0f;
        const float icy = m_cutLaneRect.top + 20.0f;
        setBrush(colAccent);
        m_renderTarget->DrawLine({icx - 5.2f, icy - 2.6f}, {icx - 5.2f, icy + 2.6f}, brush.Get(), 1.5f, roundStroke.Get());
        m_renderTarget->DrawLine({icx - 1.8f, icy - 5.6f}, {icx - 1.8f, icy + 5.6f}, brush.Get(), 1.5f, roundStroke.Get());
        m_renderTarget->DrawLine({icx + 1.8f, icy - 3.8f}, {icx + 1.8f, icy + 3.8f}, brush.Get(), 1.5f, roundStroke.Get());
        m_renderTarget->DrawLine({icx + 5.2f, icy - 1.8f}, {icx + 5.2f, icy + 1.8f}, brush.Get(), 1.5f, roundStroke.Get());
        drawTextAligned(isZh ? L"视频/音轨" : L"Video/Aud", m_captionFmt.Get(),
                        D2D1::RectF(tlCard.left + 10.0f, m_cutLaneRect.top + 28.0f, laneLeft - 6.0f, m_cutLaneRect.bottom - 4.0f),
                        colSecondary);
    }

    const D2D1_COLOR_F grooveFill = dark
        ? D2D1::ColorF(0.036f, 0.048f, 0.076f, 0.94f)
        : D2D1::ColorF(0.928f, 0.942f, 0.964f, 0.92f);
    const D2D1_COLOR_F grooveBorder = dark
        ? D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.065f)
        : D2D1::ColorF(0.08f, 0.13f, 0.22f, 0.075f);
    setBrush(grooveFill);
    m_renderTarget->FillRoundedRectangle(D2D1::RoundedRect(m_cutLaneRect, 7.5f, 7.5f), brush.Get());
    setBrush(grooveBorder);
    m_renderTarget->DrawRoundedRectangle(D2D1::RoundedRect(m_cutLaneRect, 7.5f, 7.5f), brush.Get(), 0.85f);

    const float lanePlayX = mapKeptSecToTrackX(m_playheadSec, laneLeft, trackW, keptDurSec);
    const int64_t rawTotalWaveMs = (std::max<int64_t>)(
        100, static_cast<int64_t>(std::llround(m_durationSec * 1000.0)));
    m_renderTarget->PushAxisAlignedClip(m_cutLaneRect, D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);
    int64_t accKeptMs = 0;
    for (size_t i = 0; i < m_videoClips.size(); ++i) {
        const auto& clip = m_videoClips[i];
        const int64_t cDurMs = clip.keptDurationMs();
        const float sx = mapKeptSecToTrackX(static_cast<double>(accKeptMs) / 1000.0, laneLeft, trackW, keptDurSec);
        const float ex = mapKeptSecToTrackX(static_cast<double>(accKeptMs + cDurMs) / 1000.0, laneLeft, trackW, keptDurSec);
        accKeptMs += cDurMs;

        if (ex <= laneLeft + 1.0f || sx >= laneRight - 1.0f) {
            continue;
        }
        const float gapL = (i == 0) ? 1.5f : 2.2f;
        const float gapR = (i + 1 == m_videoClips.size()) ? 1.5f : 2.2f;
        const D2D1_RECT_F clipRc = D2D1::RectF(
            sx + gapL, m_cutLaneRect.top + 1.5f,
            (std::max)(sx + gapL + 8.0f, ex - gapR), m_cutLaneRect.bottom - 1.5f);
        const D2D1_RECT_F hitClipRc = D2D1::RectF(
            (std::max)(laneLeft, clipRc.left), clipRc.top,
            (std::min)(laneRight, clipRc.right), clipRc.bottom);
        if (hitClipRc.right > hitClipRc.left + 1.0f) {
            m_hitZones.push_back({Hit_VideoClipBase + static_cast<int>(i), hitClipRc, static_cast<int>(i)});
        }

        renderCinemaFilmstripAndMountainWaveformClip(
            m_d2dFactory.Get(), m_renderTarget.Get(), brush.Get(),
            m_captionFmt.Get(), m_monoFmt.Get(),
            clipRc, laneLeft, laneRight, i, m_videoClips.size(), clip, rawTotalWaveMs,
            lanePlayX, m_selectedClipIdx == static_cast<int>(i),
            dark, isZh, m_filmstripBitmaps,
            m_timeline.audioWaveformPeaks, m_waveformProfile, roundStroke.Get());
    }
    m_renderTarget->PopAxisAlignedClip();

    // 轨道 2: 低饱和宝石深靛蓝自动运镜轨 (128.5..154.5)
    m_zoomLaneRect = D2D1::RectF(laneLeft, tlCard.top + 128.5f, laneRight, tlCard.top + 154.5f);
    drawTextAligned(isZh ? L"自动运镜" : L"Auto Zoom", m_captionFmt.Get(),
                    D2D1::RectF(tlCard.left + 10.0f, m_zoomLaneRect.top, laneLeft - 6.0f, m_zoomLaneRect.bottom), colSecondary);
    m_hitZones.push_back({Hit_ZoomLaneEmpty, m_zoomLaneRect, -1});
    setBrush(grooveFill);
    m_renderTarget->FillRoundedRectangle(D2D1::RoundedRect(m_zoomLaneRect, 5.5f, 5.5f), brush.Get());
    setBrush(grooveBorder);
    m_renderTarget->DrawRoundedRectangle(D2D1::RoundedRect(m_zoomLaneRect, 5.5f, 5.5f), brush.Get(), 0.8f);

    bool drewAnyZoom = false;
    m_renderTarget->PushAxisAlignedClip(m_zoomLaneRect, D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);
    for (size_t i = 0; i < m_timeline.zoomSegments.size(); ++i) {
        const auto& seg = m_timeline.zoomSegments[i];
        const int64_t ks = StudioTimelineAnalyzer::mapSourceMsToKeptMsViaClips(m_videoClips, seg.startMs);
        const int64_t ke = StudioTimelineAnalyzer::mapSourceMsToKeptMsViaClips(m_videoClips, seg.endMs);
        if (ke <= ks + 30) continue;
        drewAnyZoom = true;

        const float sx = mapKeptSecToTrackX(static_cast<double>(ks) / 1000.0, laneLeft, trackW, keptDurSec);
        const float ex = mapKeptSecToTrackX(static_cast<double>(ke) / 1000.0, laneLeft, trackW, keptDurSec);
        const D2D1_RECT_F segRc = D2D1::RectF(sx, m_zoomLaneRect.top + 2.0f, (std::max)(sx + 16.0f, ex), m_zoomLaneRect.bottom - 2.0f);
        if (segRc.right <= laneLeft + 1.0f || segRc.left >= laneRight - 1.0f) continue;

        const D2D1_RECT_F visSegRc = D2D1::RectF(
            (std::max)(laneLeft, segRc.left), segRc.top,
            (std::min)(laneRight, segRc.right), segRc.bottom);
        if (visSegRc.right > visSegRc.left + 1.0f) {
            m_hitZones.push_back({Hit_ZoomSegmentBase + static_cast<int>(i), visSegRc, static_cast<int>(i)});
        }
        const bool sel = (m_selectedZoomIdx == static_cast<int>(i));

        if (seg.enabled && m_config.autoZoomEnabled) {
            fillLinearVGradRounded(
                m_renderTarget.Get(), segRc, 5.0f,
                D2D1::ColorF(0.23f, 0.44f, 0.86f, sel ? 0.98f : 0.90f),
                D2D1::ColorF(0.16f, 0.33f, 0.72f, sel ? 0.98f : 0.90f));
            setBrush(D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.24f));
            m_renderTarget->DrawLine({segRc.left + 5.0f, segRc.top + 0.8f},
                                     {segRc.right - 5.0f, segRc.top + 0.8f}, brush.Get(), 0.85f);
        } else {
            setBrush(D2D1::ColorF(0.45f, 0.52f, 0.64f, 0.24f));
            m_renderTarget->FillRoundedRectangle(D2D1::RoundedRect(segRc, 5.0f, 5.0f), brush.Get());
        }
        if (sel) {
            setBrush(D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.94f));
            m_renderTarget->DrawRoundedRectangle(D2D1::RoundedRect(segRc, 5.0f, 5.0f), brush.Get(), 1.4f);
        }
        const float zw = visSegRc.right - visSegRc.left;
        if (segRc.right - segRc.left >= 24.0f) {
            setBrush(D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.42f));
            m_renderTarget->DrawLine({segRc.left + 3.4f, segRc.top + 5.8f}, {segRc.left + 3.4f, segRc.bottom - 5.8f}, brush.Get(), 1.15f);
            m_renderTarget->DrawLine({segRc.right - 3.4f, segRc.top + 5.8f}, {segRc.right - 3.4f, segRc.bottom - 5.8f}, brush.Get(), 1.15f);
        }
        if (zw >= 42.0f) {
            const float effZoom = seg.manualInserted ? seg.targetZoom : m_config.defaultZoomFactor;
            wchar_t zLbl[32]{};
            if (zw >= 74.0f) {
                std::swprintf(zLbl, std::size(zLbl), isZh ? L"运镜 %.1fx" : L"Zoom %.1fx", effZoom);
            } else {
                std::swprintf(zLbl, std::size(zLbl), L"%.1fx", effZoom);
            }
            drawTextAligned(zLbl, m_captionFmt.Get(), visSegRc,
                            D2D1::ColorF(1, 1, 1, 0.96f), DWRITE_TEXT_ALIGNMENT_CENTER);
        }
    }
    m_renderTarget->PopAxisAlignedClip();
    if (!drewAnyZoom) {
        drawTextAligned(isZh ? L"点击此轨空白处直接插入自动运镜" : L"Click anywhere on track to add zoom",
                        m_captionFmt.Get(),
                        D2D1::RectF(laneLeft + 8.0f, m_zoomLaneRect.top, laneRight - 8.0f, m_zoomLaneRect.bottom),
                        D2D1::ColorF(colSecondary.r, colSecondary.g, colSecondary.b, 0.72f));
    }

    // 轨道 3: 高定深石墨钛灰微晶标注与字幕轨 (158.0..184.0)
    m_annLaneRect = D2D1::RectF(laneLeft, tlCard.top + 158.0f, laneRight, tlCard.top + 184.0f);
    drawTextAligned(isZh ? L"标注/字幕" : L"Mark", m_captionFmt.Get(),
                    D2D1::RectF(tlCard.left + 10.0f, m_annLaneRect.top, laneLeft - 6.0f, m_annLaneRect.bottom), colSecondary);
    setBrush(grooveFill);
    m_renderTarget->FillRoundedRectangle(D2D1::RoundedRect(m_annLaneRect, 5.5f, 5.5f), brush.Get());
    setBrush(grooveBorder);
    m_renderTarget->DrawRoundedRectangle(D2D1::RoundedRect(m_annLaneRect, 5.5f, 5.5f), brush.Get(), 0.8f);

    bool drewAnyAnnOrCap = false;
    m_renderTarget->PushAxisAlignedClip(m_annLaneRect, D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);
    for (const auto& cap : m_timeline.captions) {
        const int64_t ks = StudioTimelineAnalyzer::mapSourceMsToKeptMsViaClips(m_videoClips, cap.startMs);
        const int64_t ke = StudioTimelineAnalyzer::mapSourceMsToKeptMsViaClips(m_videoClips, cap.endMs);
        if (ke <= ks + 30) continue;
        drewAnyAnnOrCap = true;
        const float sx = mapKeptSecToTrackX(static_cast<double>(ks) / 1000.0, laneLeft, trackW, keptDurSec);
        const float ex = mapKeptSecToTrackX(static_cast<double>(ke) / 1000.0, laneLeft, trackW, keptDurSec);
        if (ex <= laneLeft + 1.0f || sx >= laneRight - 1.0f) continue;
        setBrush(m_config.autoCaptionsEnabled
            ? D2D1::ColorF(0.20f, 0.56f, 0.88f, 0.72f)
            : D2D1::ColorF(0.45f, 0.52f, 0.64f, 0.25f));
        m_renderTarget->FillRoundedRectangle(
            D2D1::RoundedRect(D2D1::RectF(sx, m_annLaneRect.top + 14.0f, (std::max)(sx + 8.0f, ex), m_annLaneRect.bottom - 2.0f), 2.5f, 2.5f),
            brush.Get());
    }

    std::vector<int> stepBadgeOrderRank;
    StudioTimelineAnalyzer::resequenceAutoStepBadges(
        m_timeline.annotations, m_videoClips, &stepBadgeOrderRank);

    auto buildAnnDisplayLabel = [&](size_t annIdx, float availPx, bool forTooltip) -> std::wstring {
        const int rank = (annIdx < stepBadgeOrderRank.size()) ? stepBadgeOrderRank[annIdx] : 0;
        return StudioTimelineAnalyzer::formatTimelineAnnotationLabel(
            m_timeline.annotations[annIdx], rank, availPx, forTooltip, isZh);
    };

    int hoveredTrackAnnIdx = -1;
    D2D1_RECT_F hoveredTrackAnnRect{};

    for (size_t i = 0; i < m_timeline.annotations.size(); ++i) {
        const auto& ann = m_timeline.annotations[i];
        const int64_t ks = StudioTimelineAnalyzer::mapSourceMsToKeptMsViaClips(m_videoClips, ann.startMs);
        const int64_t ke = StudioTimelineAnalyzer::mapSourceMsToKeptMsViaClips(m_videoClips, ann.endMs);
        if (ke <= ks + 30) continue;
        drewAnyAnnOrCap = true;

        const float sx = mapKeptSecToTrackX(static_cast<double>(ks) / 1000.0, laneLeft, trackW, keptDurSec);
        const float ex = mapKeptSecToTrackX(static_cast<double>(ke) / 1000.0, laneLeft, trackW, keptDurSec);
        const D2D1_RECT_F arc = D2D1::RectF(sx, m_annLaneRect.top + 2.0f, (std::max)(sx + 18.0f, ex), m_annLaneRect.bottom - 2.0f);
        if (arc.right <= laneLeft + 1.0f || arc.left >= laneRight - 1.0f) continue;

        const D2D1_RECT_F visArc = D2D1::RectF(
            (std::max)(laneLeft, arc.left), arc.top,
            (std::min)(laneRight, arc.right), arc.bottom);
        const int hitId = Hit_AnnSegmentBase + static_cast<int>(i);
        if (visArc.right > visArc.left + 1.0f) {
            m_hitZones.push_back({hitId, visArc, static_cast<int>(i)});
        }
        const bool sel = (m_selectedAnnIdx == static_cast<int>(i));
        if (m_hoveredHitId == hitId) {
            hoveredTrackAnnIdx = static_cast<int>(i);
            hoveredTrackAnnRect = visArc;
        }

        if (ann.enabled) {
            fillLinearVGradRounded(
                m_renderTarget.Get(), arc, 5.0f,
                dark ? D2D1::ColorF(0.22f, 0.27f, 0.38f, sel ? 0.98f : 0.92f)
                     : D2D1::ColorF(0.21f, 0.26f, 0.36f, sel ? 0.96f : 0.90f),
                dark ? D2D1::ColorF(0.15f, 0.19f, 0.28f, sel ? 0.98f : 0.92f)
                     : D2D1::ColorF(0.13f, 0.17f, 0.25f, sel ? 0.96f : 0.90f));
            setBrush(D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.18f));
            m_renderTarget->DrawLine({arc.left + 5.0f, arc.top + 0.8f},
                                     {arc.right - 5.0f, arc.top + 0.8f}, brush.Get(), 0.85f);
        } else {
            setBrush(D2D1::ColorF(0.45f, 0.52f, 0.64f, 0.24f));
            m_renderTarget->FillRoundedRectangle(D2D1::RoundedRect(arc, 5.0f, 5.0f), brush.Get());
        }
        if (sel) {
            setBrush(hexToD2DColor(ann.colorHex, 0.95f));
            m_renderTarget->DrawRoundedRectangle(D2D1::RoundedRect(arc, 5.0f, 5.0f), brush.Get(), 1.45f);
        }

        const float aw = visArc.right - visArc.left;
        if (arc.right - arc.left >= 24.0f) {
            setBrush(D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.36f));
            m_renderTarget->DrawLine({arc.left + 3.4f, arc.top + 5.8f}, {arc.left + 3.4f, arc.bottom - 5.8f}, brush.Get(), 1.15f);
            m_renderTarget->DrawLine({arc.right - 3.4f, arc.top + 5.8f}, {arc.right - 3.4f, arc.bottom - 5.8f}, brush.Get(), 1.15f);
        }

        if (aw >= 22.0f) {
            m_renderTarget->PushAxisAlignedClip(
                D2D1::RectF(visArc.left + 4.5f, visArc.top, visArc.right - 4.5f, visArc.bottom),
                D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);

            float textLeft = visArc.left + 5.5f;
            if (aw >= 56.0f) {
                const float dotCy = (visArc.top + visArc.bottom) * 0.5f;
                const float dotCx = visArc.left + 9.5f;
                setBrush(D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.92f));
                m_renderTarget->FillEllipse(D2D1::Ellipse({dotCx, dotCy}, 3.4f, 3.4f), brush.Get());
                setBrush(hexToD2DColor(ann.colorHex, 1.0f));
                m_renderTarget->FillEllipse(D2D1::Ellipse({dotCx, dotCy}, 2.5f, 2.5f), brush.Get());
                textLeft = visArc.left + 15.0f;
            }

            const std::wstring lbl = buildAnnDisplayLabel(i, aw, false);
            drawTextAligned(
                lbl, m_captionFmt.Get(),
                D2D1::RectF(textLeft, visArc.top, visArc.right - 5.0f, visArc.bottom),
                D2D1::ColorF(0.96f, 0.98f, 1.0f, 0.96f),
                (aw < 56.0f) ? DWRITE_TEXT_ALIGNMENT_CENTER : DWRITE_TEXT_ALIGNMENT_LEADING);

            m_renderTarget->PopAxisAlignedClip();
        }
    }
    m_renderTarget->PopAxisAlignedClip();

    if (!drewAnyAnnOrCap) {
        drawTextAligned(isZh ? L"从左侧「标注」点击或拖入图标，名称与步骤序号将在此显示"
                             : L"Add or drag marks from Annotations panel",
                        m_captionFmt.Get(),
                        D2D1::RectF(laneLeft + 8.0f, m_annLaneRect.top, laneRight - 8.0f, m_annLaneRect.bottom),
                        D2D1::ColorF(colSecondary.r, colSecondary.g, colSecondary.b, 0.72f));
    }

    // 放大视野下的微晶水平定位滚动槽（直观显示当前查看区间在全片中的位置，支持直接拖拽平移视野）
    if (m_timelineZoom > 1.015f && keptDurSec > 0.2) {
        const D2D1_RECT_F panTrk = D2D1::RectF(laneLeft, tlCard.top + 187.5f, laneRight, tlCard.top + 192.5f);
        m_sliderTrackRects[static_cast<size_t>(SliderDragKind::TimelinePan)] = panTrk;
        m_hitZones.push_back({Hit_TimelinePanBar, D2D1::RectF(laneLeft, tlCard.top + 185.0f, laneRight, tlCard.bottom - 1.0f), -1});

        setBrush(dark ? D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.08f) : D2D1::ColorF(0.08f, 0.14f, 0.24f, 0.08f));
        m_renderTarget->FillRoundedRectangle(D2D1::RoundedRect(panTrk, 2.5f, 2.5f), brush.Get());

        const float thumbFrac = std::clamp(static_cast<float>(visSpanSec / keptDurSec), 0.04f, 1.0f);
        const float thumbW = (std::max)(24.0f, trackW * thumbFrac);
        const double maxScroll = (std::max)(1e-5, keptDurSec - visSpanSec);
        const float scrollRatio = std::clamp(static_cast<float>(m_timelineViewStartSec / maxScroll), 0.0f, 1.0f);
        const float thumbL = panTrk.left + (trackW - thumbW) * scrollRatio;
        const bool hovPan = (m_hoveredHitId == Hit_TimelinePanBar || m_activeSlider == SliderDragKind::TimelinePan);
        setBrush(hovPan ? colAccent : D2D1::ColorF(accentRgb.r, accentRgb.g, accentRgb.b, dark ? 0.65f : 0.58f));
        m_renderTarget->FillRoundedRectangle(
            D2D1::RoundedRect(D2D1::RectF(thumbL, panTrk.top, thumbL + thumbW, panTrk.bottom), 2.5f, 2.5f),
            brush.Get());
    }

    // 悬停实时掠览游标细线 (Live Skimmer Line) + 石墨微晶时间胶囊
    if (m_hoverSkimming && keptDurSec > 1e-4) {
        const float skimX = mapKeptSecToTrackX(m_hoverSkimSec, laneLeft, trackW, keptDurSec);
        if (skimX >= laneLeft - 1.5f && skimX <= laneRight + 1.5f) {
            setBrush(dark ? D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.56f) : D2D1::ColorF(0.15f, 0.22f, 0.36f, 0.48f));
            m_renderTarget->DrawLine({skimX, m_cutLaneRect.top - 6.0f}, {skimX, m_annLaneRect.bottom + 2.0f}, brush.Get(), 1.0f);

            const std::wstring skTxt = formatCompactDurationBadge(m_hoverSkimSec, isZh);
            const float skHalfW = (m_hoverSkimSec >= 120.0) ? 32.0f : 22.0f;
            const float skLeft = std::clamp(skimX - skHalfW, laneLeft, laneRight - skHalfW * 2.0f);
            const D2D1_RECT_F badgeRc = D2D1::RectF(
                skLeft, tlCard.top + 40.5f, skLeft + skHalfW * 2.0f, tlCard.top + 56.5f);
            setBrush(dark ? D2D1::ColorF(0.15f, 0.20f, 0.30f, 0.96f) : D2D1::ColorF(0.12f, 0.16f, 0.24f, 0.92f));
            m_renderTarget->FillRoundedRectangle(D2D1::RoundedRect(badgeRc, 4.0f, 4.0f), brush.Get());
            drawTextAligned(skTxt, m_monoFmt.Get(), badgeRc, D2D1::ColorF(1, 1, 1, 0.98f), DWRITE_TEXT_ALIGNMENT_CENTER);
        }
    }

    // 贯穿三轨的 Screen Studio / Final Cut 级精密光学细针播放头 (Precision Crimson Optical Playhead)
    if (lanePlayX >= laneLeft - 2.0f && lanePlayX <= laneRight + 2.0f) {
        // 柔和光学微影（摒弃粗黑塑料重描边）
        setBrush(D2D1::ColorF(0.02f, 0.04f, 0.10f, dark ? 0.34f : 0.16f));
        m_renderTarget->DrawLine({lanePlayX + 0.6f, m_cutLaneRect.top - 2.0f},
                                 {lanePlayX + 0.6f, m_annLaneRect.bottom + 2.5f}, brush.Get(), 2.2f);

        // 1.35px 锐利朱砂红光学指针线
        const D2D1_COLOR_F needleCol = D2D1::ColorF(0.88f, 0.14f, 0.28f, 0.98f); // #E12447 徕卡精密绯红
        setBrush(needleCol);
        m_renderTarget->DrawLine({lanePlayX, m_cutLaneRect.top - 2.5f},
                                 {lanePlayX, m_annLaneRect.bottom + 2.5f}, brush.Get(), 1.35f);

        // 顶部圆润微晶倒水滴针帽 (8.4 x 9.6px)
        ComPtr<ID2D1PathGeometry> capGeo;
        if (m_d2dFactory && SUCCEEDED(m_d2dFactory->CreatePathGeometry(capGeo.GetAddressOf())) && capGeo) {
            ComPtr<ID2D1GeometrySink> sink;
            if (SUCCEEDED(capGeo->Open(sink.GetAddressOf())) && sink) {
                const float ct = m_cutLaneRect.top - 10.2f;
                const float hw = 4.2f;
                sink->BeginFigure({lanePlayX - hw + 1.8f, ct}, D2D1_FIGURE_BEGIN_FILLED);
                sink->AddLine({lanePlayX + hw - 1.8f, ct});
                sink->AddBezier(D2D1::BezierSegment(
                    {lanePlayX + hw, ct}, {lanePlayX + hw, ct + 1.8f}, {lanePlayX + hw, ct + 3.6f}));
                sink->AddBezier(D2D1::BezierSegment(
                    {lanePlayX + hw, ct + 6.0f}, {lanePlayX + 1.4f, ct + 8.0f}, {lanePlayX, ct + 9.6f}));
                sink->AddBezier(D2D1::BezierSegment(
                    {lanePlayX - 1.4f, ct + 8.0f}, {lanePlayX - hw, ct + 6.0f}, {lanePlayX - hw, ct + 3.6f}));
                sink->AddBezier(D2D1::BezierSegment(
                    {lanePlayX - hw, ct + 1.8f}, {lanePlayX - hw, ct}, {lanePlayX - hw + 1.8f, ct}));
                sink->EndFigure(D2D1_FIGURE_END_CLOSED);
                sink->Close();
                setBrush(needleCol);
                m_renderTarget->FillGeometry(capGeo.Get(), brush.Get());
                setBrush(D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.85f));
                m_renderTarget->DrawGeometry(capGeo.Get(), brush.Get(), 0.8f);
            }
        }
    }

    // 悬停在时间轴标注轨道块上时，在轨道块正上方浮现完整标注名称/步骤序号与起止秒数气泡
    if (!m_exportModalOpen && !m_draggingPresetToStage &&
        m_trackDragKind == TrackDragKind::None &&
        hoveredTrackAnnIdx >= 0 &&
        hoveredTrackAnnIdx < static_cast<int>(m_timeline.annotations.size())) {
        const auto& hann = m_timeline.annotations[static_cast<size_t>(hoveredTrackAnnIdx)];
        const int64_t hks = StudioTimelineAnalyzer::mapSourceMsToKeptMsViaClips(m_videoClips, hann.startMs);
        const int64_t hke = StudioTimelineAnalyzer::mapSourceMsToKeptMsViaClips(m_videoClips, hann.endMs);
        wchar_t timeSpanBuf[48]{};
        std::swprintf(timeSpanBuf, std::size(timeSpanBuf), L" (%.1fs - %.1fs)", hks / 1000.0, hke / 1000.0);
        const std::wstring fullTip = buildAnnDisplayLabel(static_cast<size_t>(hoveredTrackAnnIdx), 999.0f, true) + timeSpanBuf;

        float estW = 0.0f;
        for (wchar_t wc : fullTip) estW += (wc > 0x7F) ? 12.2f : 6.8f;
        const float tipW = std::clamp(estW + 22.0f, 84.0f, W - 24.0f);
        const float tipH = 24.0f;
        const float annTipCx = (hoveredTrackAnnRect.left + hoveredTrackAnnRect.right) * 0.5f;
        const float tipL = std::clamp(annTipCx - tipW * 0.5f, laneLeft, laneRight - tipW);
        const float tipT = hoveredTrackAnnRect.top - tipH - 6.0f;
        const D2D1_RECT_F tipRc = D2D1::RectF(tipL, tipT, tipL + tipW, tipT + tipH);

        setBrush(D2D1::ColorF(0.02f, 0.04f, 0.09f, dark ? 0.52f : 0.22f));
        m_renderTarget->FillRoundedRectangle(
            D2D1::RoundedRect(D2D1::RectF(tipRc.left - 1.0f, tipRc.top + 2.0f, tipRc.right + 1.0f, tipRc.bottom + 3.0f), 6.5f, 6.5f),
            brush.Get());
        setBrush(dark ? D2D1::ColorF(0.13f, 0.17f, 0.26f, 0.98f) : D2D1::ColorF(0.11f, 0.15f, 0.24f, 0.96f));
        m_renderTarget->FillRoundedRectangle(D2D1::RoundedRect(tipRc, 6.0f, 6.0f), brush.Get());
        setBrush(D2D1::ColorF(0.95f, 0.56f, 0.12f, 0.72f));
        m_renderTarget->DrawRoundedRectangle(D2D1::RoundedRect(tipRc, 6.0f, 6.0f), brush.Get(), 1.0f);
        drawTextAligned(fullTip, m_captionFmt.Get(),
                        D2D1::RectF(tipRc.left + 8.0f, tipRc.top, tipRc.right - 8.0f, tipRc.bottom),
                        D2D1::ColorF(0.98f, 0.99f, 1.0f, 0.98f), DWRITE_TEXT_ALIGNMENT_CENTER);
    }

    if (!m_exportModalOpen && m_hoveredCoachId != 0 && !m_draggingPresetToStage) {
        renderCoachTooltipOverlay(W, H, rightAreaLeft, dark, isZh, brush.Get());
    }

    // 从左侧标注面板按住形状图标拖往右侧舞台时的半透明跟随拖拽预览胶囊
    if (m_draggingPresetToStage && m_pressedPresetCardIdx >= 0 && m_pressedPresetCardIdx < 19) {
        const std::wstring dragTitle = std::wstring(L"+ ") + annotationCardDisplayName(m_pressedPresetCardIdx, isZh);
        float estW = 0.0f;
        for (wchar_t wc : dragTitle) estW += (wc > 0x7F) ? 12.6f : 7.2f;
        const float ghostW = std::clamp(estW + 24.0f, 76.0f, 180.0f);
        const float ghostH = 28.0f;
        const D2D1_RECT_F ghostRc = D2D1::RectF(
            std::clamp(m_lastMouseLx + 12.0f, 6.0f, W - ghostW - 6.0f),
            std::clamp(m_lastMouseLy - ghostH * 0.5f, 6.0f, H - ghostH - 6.0f),
            std::clamp(m_lastMouseLx + 12.0f, 6.0f, W - ghostW - 6.0f) + ghostW,
            std::clamp(m_lastMouseLy - ghostH * 0.5f, 6.0f, H - ghostH - 6.0f) + ghostH);

        // 若已拖入右侧视频画布范围，同时画出落点十字微光指示
        float dropNx = 0.5f, dropNy = 0.5f;
        if (pointInRectF(m_lastBitmapDrawRect, m_lastMouseLx, m_lastMouseLy) &&
            mapStagePointToSourceNorm(m_lastMouseLx, m_lastMouseLy, dropNx, dropNy)) {
            setBrush(D2D1::ColorF(colAccent.r, colAccent.g, colAccent.b, 0.85f));
            m_renderTarget->DrawEllipse(D2D1::Ellipse({m_lastMouseLx, m_lastMouseLy}, 14.0f, 14.0f), brush.Get(), 1.5f);
        }

        setBrush(D2D1::ColorF(0.02f, 0.04f, 0.09f, 0.34f));
        m_renderTarget->FillRoundedRectangle(
            D2D1::RoundedRect(D2D1::RectF(ghostRc.left, ghostRc.top + 3.0f, ghostRc.right, ghostRc.bottom + 3.0f), 8.0f, 8.0f),
            brush.Get());
        setBrush(D2D1::ColorF(colAccent.r, colAccent.g, colAccent.b, 0.92f));
        m_renderTarget->FillRoundedRectangle(D2D1::RoundedRect(ghostRc, 7.5f, 7.5f), brush.Get());
        setBrush(D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.85f));
        m_renderTarget->DrawRoundedRectangle(D2D1::RoundedRect(ghostRc, 7.5f, 7.5f), brush.Get(), 1.1f);
        drawTextAligned(dragTitle, m_captionFmt.Get(), ghostRc, D2D1::ColorF(1, 1, 1, 1), DWRITE_TEXT_ALIGNMENT_CENTER);
    }

    // 非最大化态窗口右下角绘制三道细腻斜向防滑缩放暗纹，直观提示窗口四边四角可自由拉伸缩放
    if (!m_isMaximized && !m_exportModalOpen) {
        setBrush(dark ? D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.22f) : D2D1::ColorF(0.12f, 0.18f, 0.30f, 0.22f));
        for (int g = 0; g < 3; ++g) {
            const float off = 4.5f + g * 3.8f;
            m_renderTarget->DrawLine({W - 3.5f - off, H - 3.5f}, {W - 3.5f, H - 3.5f - off},
                                     brush.Get(), 1.15f, roundStroke.Get());
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

    if (m_preparingSession) {
        InvalidateRect(m_hwnd, nullptr, FALSE);
        return;
    }

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

    if (m_copyFeedbackUntilTick > 0 && GetTickCount64() >= m_copyFeedbackUntilTick) {
        m_copyFeedbackUntilTick = 0;
        InvalidateRect(m_hwnd, nullptr, FALSE);
    }

    if (m_exporting.load(std::memory_order_acquire) || m_editingExportName ||
        m_editingAnnotationText || m_editingWatermarkText) {
        InvalidateRect(m_hwnd, nullptr, FALSE);
        return;
    }

    // 若用户刚刚在左侧切换了点击动效，在暂停态亦以 60FPS 逐帧驱动舞台光标完成实时点击动效演示
    if (m_clickDemoStartTick > 0) {
        const ULONGLONG now64 = GetTickCount64();
        if (now64 >= m_clickDemoStartTick && (now64 - m_clickDemoStartTick) <= 460) {
            updateCurrentComposedBitmap();
            InvalidateRect(m_hwnd, nullptr, FALSE);
            if (!m_playing) return;
        } else {
            m_clickDemoStartTick = 0;
            updateCurrentComposedBitmap();
            InvalidateRect(m_hwnd, nullptr, FALSE);
        }
    }

    // 若用户刚添加动态重点标注，在暂停态下亦以 60FPS 逐帧驱动舞台演示 1.45s 的一笔勾勒与声呐脉冲
    if (m_annDemoStartTick > 0) {
        const ULONGLONG now64 = GetTickCount64();
        if (now64 >= m_annDemoStartTick && (now64 - m_annDemoStartTick) <= 1450) {
            updateCurrentComposedBitmap();
            InvalidateRect(m_hwnd, nullptr, FALSE);
            if (!m_playing) return;
        } else {
            m_annDemoStartTick = 0;
            updateCurrentComposedBitmap();
            InvalidateRect(m_hwnd, nullptr, FALSE);
        }
    }

    // 悬停在时间轴上实时掠览看点时不推进主播放头；若鼠标已停稳 >= 65ms 且上一帧使用的是代理图，自动无感升格为 100% 原生全清帧
    if (m_hoverSkimming) {
        if (m_lastComposedUsedProxy && m_hiResReader.isOpen() &&
            (GetTickCount64() - m_lastFastScrubTick) >= 65) {
            m_hoverSkimming = false;
            const double keepSkim = m_hoverSkimSec;
            m_hoverSkimming = true;
            m_hoverSkimSec = keepSkim;
            cv::Mat nativeBgr;
            if (m_hiResReader.decodeFrameAt(currentDisplayedSourceSec(), nativeBgr) && !nativeBgr.empty()) {
                m_lastFastScrubTick = GetTickCount64();
                updateCurrentComposedBitmap();
                m_lastComposedUsedProxy = false;
                InvalidateRect(m_hwnd, nullptr, FALSE);
                return;
            }
        }
        if (!m_exportModalOpen && m_hoveredCoachId != 0) {
            InvalidateRect(m_hwnd, nullptr, FALSE);
        }
        return;
    }

    if (!m_playing && m_lastComposedUsedProxy && !m_scrubbingTimeline &&
        m_trackDragKind == TrackDragKind::None && !m_inInteractiveSizing &&
        (GetTickCount64() - m_lastFastScrubTick) >= 65) {
        updateCurrentComposedBitmap();
        InvalidateRect(m_hwnd, nullptr, FALSE);
        return;
    }

    const double keptDurSec = computeEffectiveDurationSec();
    if (m_playing && !m_exportModalOpen && !m_scrubbingTimeline &&
        m_trackDragKind == TrackDragKind::None && m_stageAnnDrag == StageAnnDragKind::None &&
        !m_draggingZoomFocus && !m_draggingPresetToStage && keptDurSec > 0.05) {
        m_playheadSec += dt;
        if (m_playheadSec >= keptDurSec) {
            m_playheadSec = 0.0;
            if (m_timelineZoom > 1.01f) {
                m_timelineViewStartSec = 0.0;
            }
        } else if (m_timelineZoom > 1.01f) {
            const double visSpan = visibleTimelineSpanSec(keptDurSec);
            if (m_playheadSec < m_timelineViewStartSec ||
                m_playheadSec > m_timelineViewStartSec + visSpan * 0.92) {
                m_timelineViewStartSec = m_playheadSec - visSpan * 0.10;
                clampTimelineViewWindow(keptDurSec);
            }
        }
        updateCurrentComposedBitmap();
        InvalidateRect(m_hwnd, nullptr, FALSE);
        return;
    }

    // 暂停态下：若鼠标正悬停在底部 7 大功能按钮教学卡或左侧动态标注库卡片上，零解码开销驱动 60FPS D2D 矢量微剧场
    const bool hoverOnAnnCard = (m_activeTab == LeftRailTab::Annotations &&
                                 m_hoveredHitId >= Hit_AnnCardBase &&
                                 m_hoveredHitId < Hit_AnnCardBase + 19);
    if (!m_exportModalOpen && (m_hoveredCoachId != 0 || hoverOnAnnCard)) {
        InvalidateRect(m_hwnd, nullptr, FALSE);
    }
}

void StudioPreviewWindow::startAsyncExport() {
    if (m_exporting.exchange(true, std::memory_order_acq_rel)) return;

    m_playing = false;
    m_preparingSession = false;
    m_hoverSkimming = false;
    m_exportCompletedCard = false;
    m_editingExportName = false;
    m_openDropdown = ExportDropdownKind::None;
    m_cancelExport.store(false, std::memory_order_release);
    m_exportProgress.store(0.01, std::memory_order_release);
    syncCutSegmentsFromVideoClips();
    persistStudioConfigToSettings(m_config);
    persistExportPreferencesIfChecked();
    InvalidateRect(m_hwnd, nullptr, FALSE);

    joinExportWorkerIfDone(true);

    std::string sourceMasterPath = m_masterVideoPath.empty() ? m_rawVideoPath : m_masterVideoPath;
    const std::string finalPath = buildFinalExportPathUtf8();
    {
        const std::filesystem::path srcFs(tools3000::core::WinUtils::utf8ToWstring(sourceMasterPath));
        const std::filesystem::path dstFs(tools3000::core::WinUtils::utf8ToWstring(finalPath));
        std::error_code eqEc;
        const bool sameTarget =
            (std::filesystem::exists(srcFs, eqEc) && std::filesystem::exists(dstFs, eqEc) &&
             std::filesystem::equivalent(srcFs, dstFs, eqEc)) ||
            (normalizeLowerFsKey(srcFs) == normalizeLowerFsKey(dstFs));
        if (sameTarget && std::filesystem::exists(srcFs, eqEc)) {
            m_hiResReader.close();
            const std::wstring isoName = std::format(
                L".studio_master_{}_{}{}",
                GetTickCount64(),
                srcFs.stem().wstring(),
                srcFs.extension().wstring());
            const std::filesystem::path isoFs = srcFs.parent_path() / isoName;
            std::error_code renEc;
            std::filesystem::rename(srcFs, isoFs, renEc);
            if (!renEc) {
                sourceMasterPath = tools3000::core::WinUtils::wstringToUtf8(isoFs.wstring());
                m_masterVideoPath = sourceMasterPath;
                m_masterIsIsolatedTemp = true;
            }
            m_hiResReader.open(m_masterVideoPath, 3200);
        }
    }

    const StudioTimelineData timelineCopy = m_timeline;
    const StudioRenderConfig configCopy = m_config;
    const StudioExportOptions exportOptsCopy = m_exportOpts;
    const double durSec = computeEffectiveDurationSec();
    const std::string stagingPath = finalPath + ".part";

    std::lock_guard lock(m_exportMutex);
    m_exportWorker = std::thread([this, sourceMasterPath, stagingPath, finalPath, timelineCopy, configCopy, exportOptsCopy, durSec]() {
        tools3000::core::TraceId::Scope traceScope;
        bool ok = StudioVideoCompositor::exportStudioMedia(
            sourceMasterPath,
            stagingPath,
            timelineCopy,
            configCopy,
            exportOptsCopy,
            [this](double p) {
                m_exportProgress.store(p, std::memory_order_relaxed);
            },
            &m_cancelExport);

        const bool canceled = m_cancelExport.load(std::memory_order_acquire);
        const std::filesystem::path stagingFs(tools3000::core::WinUtils::utf8ToWstring(stagingPath));
        const std::filesystem::path finalFs(tools3000::core::WinUtils::utf8ToWstring(finalPath));
        if (ok && !canceled) {
            std::error_code rmOldEc;
            if (std::filesystem::exists(finalFs, rmOldEc)) {
                std::filesystem::remove(finalFs, rmOldEc);
            }
            std::error_code renEc;
            std::filesystem::rename(stagingFs, finalFs, renEc);
            if (renEc) {
                ok = false;
                std::error_code cleanEc;
                std::filesystem::remove(stagingFs, cleanEc);
            }
        } else {
            std::error_code cleanEc;
            std::filesystem::remove(stagingFs, cleanEc);
        }

        tools3000::core::MainThreadDispatcher::instance().post(
            [this, ok, canceled, sourceMasterPath, finalPath, exportOptsCopy, durSec]() {
                m_exporting.store(false, std::memory_order_release);
                if (canceled) return;
                const bool isZh = tools3000::core::NativeThemePalette::isUiLanguageChinese();
                if (ok) {
                    const std::filesystem::path doneFs(tools3000::core::WinUtils::utf8ToWstring(finalPath));
                    std::error_code szEc;
                    const auto fsz = std::filesystem::file_size(doneFs, szEc);
                    m_lastExportedFileBytes = szEc ? 0ULL : static_cast<std::uint64_t>(fsz);
                    m_lastExportedFilePath = finalPath;
                    m_lastExportedFormat = exportOptsCopy.format;
                    m_lastExportedSpecLine = std::format(
                        L"{} · {} · {} FPS",
                        formatLabel(exportOptsCopy.format, isZh),
                        resolutionLabel(exportOptsCopy.resolution, isZh),
                        exportOptsCopy.fps);

                    const std::wstring normFinal = normalizeLowerFsKey(doneFs);
                    if (std::find(m_sessionExportedPathsLower.begin(), m_sessionExportedPathsLower.end(), normFinal) ==
                        m_sessionExportedPathsLower.end()) {
                        m_sessionExportedPathsLower.push_back(normFinal);
                    }
                    m_copyFeedbackUntilTick = 0;

                    LOG_INFO("StudioPreviewWindow: exported studio video successfully: {}", finalPath);
                    if (m_autoCloseAfterExport) {
                        finishExportedSessionAndClose();
                    } else {
                        m_exportModalOpen = true;
                        m_exportCompletedCard = true;
                        InvalidateRect(m_hwnd, nullptr, FALSE);
                    }
                } else {
                    LOG_WARN("StudioPreviewWindow: exportRenderedVideo failed, keeping raw video: {}", sourceMasterPath);
                    tools3000::core::EventBus::instance().publish(tools3000::core::ShowToastEvent{
                        isZh ? L"演播室合成失败，已为您安全保留原始录屏" : L"Studio export failed; raw recording preserved"});
                    InvalidateRect(m_hwnd, nullptr, FALSE);
                }
            });
    });
}

void StudioPreviewWindow::copyLastExportedToClipboard() {
    if (m_lastExportedFilePath.empty()) return;
    const std::wstring widePath = tools3000::core::WinUtils::utf8ToWstring(m_lastExportedFilePath);
    bool copied = false;
    if (m_lastExportedFormat == StudioExportFormat::GIF_WeChat ||
        m_lastExportedFormat == StudioExportFormat::GIF_HD) {
        copied = tools3000::core::WinUtils::copyGifToClipboard(widePath, m_hwnd);
    }
    if (!copied) {
        copied = tools3000::core::WinUtils::copyFileToClipboard(widePath, m_hwnd);
    }
    if (copied) {
        m_copyFeedbackUntilTick = GetTickCount64() + 1800;
        InvalidateRect(m_hwnd, nullptr, FALSE);
    }
}

void StudioPreviewWindow::finishExportedSessionAndClose() {
    if (m_exporting.load(std::memory_order_acquire)) return;
    persistStudioConfigToSettings(m_config);
    m_hiResReader.close();

    // 清理未作为最终成片输出的临时/原始无壁纸母带（保护所有已导出成片绝不误删）
    const std::string masterPath = m_masterVideoPath.empty() ? m_rawVideoPath : m_masterVideoPath;
    if (!masterPath.empty()) {
        const std::filesystem::path masterFs(tools3000::core::WinUtils::utf8ToWstring(masterPath));
        const std::wstring normMaster = normalizeLowerFsKey(masterFs);
        const bool isExportedArtifact =
            std::find(m_sessionExportedPathsLower.begin(), m_sessionExportedPathsLower.end(), normMaster) !=
            m_sessionExportedPathsLower.end();
        if (!isExportedArtifact) {
            std::error_code rmEc;
            std::filesystem::remove(masterFs, rmEc);
        }
    }

    const std::string deliveredPath = !m_lastExportedFilePath.empty() ? m_lastExportedFilePath : m_rawVideoPath;
    const double dur = computeEffectiveDurationSec();
    auto cb = std::move(m_onFinished);
    closeSessionAndCleanup();
    if (cb) {
        cb(deliveredPath, dur, true);
    }
}

void StudioPreviewWindow::closeOrKeepBySessionState() {
    if (!m_sessionExportedPathsLower.empty()) {
        finishExportedSessionAndClose();
    } else {
        keepOriginalAndClose();
    }
}

void StudioPreviewWindow::keepOriginalAndClose() {
    if (m_exporting.load(std::memory_order_acquire)) return;
    persistStudioConfigToSettings(m_config);
    m_hiResReader.close();

    std::string finalRawPath = m_rawVideoPath;
    if (m_masterIsIsolatedTemp && !m_masterVideoPath.empty()) {
        const std::filesystem::path masterFs(tools3000::core::WinUtils::utf8ToWstring(m_masterVideoPath));
        const std::filesystem::path origRawFs(tools3000::core::WinUtils::utf8ToWstring(m_rawVideoPath));
        std::error_code ec;
        if (std::filesystem::exists(masterFs, ec)) {
            if (!std::filesystem::exists(origRawFs, ec)) {
                std::filesystem::rename(masterFs, origRawFs, ec);
                if (!ec) {
                    finalRawPath = m_rawVideoPath;
                } else {
                    finalRawPath = m_masterVideoPath;
                }
            } else {
                const std::filesystem::path altRawFs =
                    origRawFs.parent_path() / (origRawFs.stem().wstring() + L"_raw" + origRawFs.extension().wstring());
                std::filesystem::rename(masterFs, altRawFs, ec);
                finalRawPath = !ec ? tools3000::core::WinUtils::wstringToUtf8(altRawFs.wstring()) : m_masterVideoPath;
            }
        }
    }

    const double dur = m_durationSec;
    auto cb = std::move(m_onFinished);
    closeSessionAndCleanup();
    if (cb) {
        cb(finalRawPath, dur, false);
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
        if (!m_isMaximized && m_logicalWidth >= 1020 && m_logicalHeight >= 650) {
            auto& cm = tools3000::core::ConfigManager::instance();
            cm.set<int>("/recording/studioWindowWidth", m_logicalWidth);
            cm.set<int>("/recording/studioWindowHeight", m_logicalHeight);
        }
        KillTimer(m_hwnd, kPlaybackTimerId);
        ShowWindow(m_hwnd, SW_HIDE);
    }
    m_playing = false;
    m_preparingSession = false;
    m_hoverSkimming = false;
    m_pendingSkimCompose = false;
    m_trackingMouseLeave = false;
    m_exportModalOpen = false;
    m_exportCompletedCard = false;
    m_editingExportName = false;
    m_userEditedExportStem = false;
    m_editingAnnotationText = false;
    m_editingWatermarkText = false;
    m_openDropdown = ExportDropdownKind::None;
    m_sessionExportedPathsLower.clear();
    m_lastExportedFilePath.clear();
    m_lastExportedFileBytes = 0;
    m_lastExportedSpecLine.clear();
    m_copyFeedbackUntilTick = 0;
    m_masterVideoPath.clear();
    m_masterIsIsolatedTemp = false;
    m_scrubbingTimeline = false;
    m_draggingWindow = false;
    m_stageAnnDrag = StageAnnDragKind::None;
    m_hoveredStageAnnIdx = -1;
    m_hoveredStageAnnPart = StageAnnDragKind::None;
    m_draggingZoomFocus = false;
    m_hoveredZoomFocusReticle = false;
    m_watermarkStageDragMode = 0;
    m_hoveredWatermarkOnStage = false;
    m_hoveredWatermarkCorner = false;
    m_lastWatermarkStageRect = D2D1::RectF(0.0f, 0.0f, 0.0f, 0.0f);
    m_pressedPresetCardIdx = -1;
    m_draggingPresetToStage = false;
    m_trackDragKind = TrackDragKind::None;
    m_activeSlider = SliderDragKind::None;
    m_timelineZoom = 1.0f;
    m_timelineViewStartSec = 0.0;
    m_clickDemoStartTick = 0;
    m_annDemoStartTick = 0;
    m_coachHoverZones.clear();
    m_hoveredCoachId = 0;
    m_coachHoverEnterTick = 0;
    m_coachLastActiveTick = 0;
    m_undoStack.clear();
    m_redoStack.clear();
    m_hiResReader.close();
    m_filmstripBitmaps.clear();
    m_filmstripDirty = true;
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
    } else if (kind == SliderDragKind::WatermarkScale) {
        m_config.watermark.scale = 0.35f + ratio * (2.80f - 0.35f);
        m_config.watermark.enabled = true;
        WatermarkRenderer::saveStudioWatermarkConfig(m_config.watermark);
    } else if (kind == SliderDragKind::WatermarkOpacity) {
        m_config.watermark.opacity = 0.04f + ratio * (1.00f - 0.04f);
        m_config.watermark.enabled = true;
        WatermarkRenderer::saveStudioWatermarkConfig(m_config.watermark);
    } else if (kind == SliderDragKind::TimelineZoom) {
        const double keptDurSec = computeEffectiveDurationSec();
        const float targetZ = std::pow(24.0f, ratio);
        setTimelineZoomAnchored(targetZ, m_playheadSec, 0.5f, keptDurSec);
        if (m_hwnd) InvalidateRect(m_hwnd, nullptr, FALSE);
        return;
    } else if (kind == SliderDragKind::TimelinePan) {
        const double keptDurSec = computeEffectiveDurationSec();
        const double visSpan = visibleTimelineSpanSec(keptDurSec);
        const float thumbFrac = std::clamp(static_cast<float>(visSpan / (std::max)(0.2, keptDurSec)), 0.04f, 1.0f);
        const float thumbW = (std::max)(24.0f, w * thumbFrac);
        const float effRange = (std::max)(1.0f, w - thumbW);
        const float panRatio = std::clamp((lx - trk.left - thumbW * 0.5f) / effRange, 0.0f, 1.0f);
        m_timelineViewStartSec = static_cast<double>(panRatio) * (std::max)(0.0, keptDurSec - visSpan);
        clampTimelineViewWindow(keptDurSec);
        if (m_hwnd) InvalidateRect(m_hwnd, nullptr, FALSE);
        return;
    }
    requestImmediatePreviewRefresh(false);
}

void StudioPreviewWindow::handleMouseWheel(float lx, float ly, int wheelDelta) {
    if (m_exportModalOpen || m_preparingSession || wheelDelta == 0) return;
    if (pointInRectF(m_timelineCardRect, lx, ly)) {
        const double keptDurSec = computeEffectiveDurationSec();
        const float laneLeft = m_cutLaneRect.left;
        const float trackW = (std::max)(1.0f, m_cutLaneRect.right - laneLeft);
        const bool shiftDown = (GetAsyncKeyState(VK_SHIFT) & 0x8000) != 0;
        if (shiftDown && m_timelineZoom > 1.01f) {
            const double visSpan = visibleTimelineSpanSec(keptDurSec);
            const double step = visSpan * 0.18 * ((wheelDelta < 0) ? 1.0 : -1.0);
            m_timelineViewStartSec += step;
            clampTimelineViewWindow(keptDurSec);
            if (m_hwnd) InvalidateRect(m_hwnd, nullptr, FALSE);
            return;
        }
        const float factor = (wheelDelta > 0) ? 1.26f : (1.0f / 1.26f);
        const bool onLanes = (lx >= laneLeft && lx <= m_cutLaneRect.right && ly >= m_allLanesHoverRect.top);
        const float anchorU = onLanes
            ? std::clamp((lx - laneLeft) / trackW, 0.0f, 1.0f)
            : 0.5f;
        const double anchorSec = onLanes
            ? mapTrackXToKeptSec(lx, laneLeft, trackW, keptDurSec)
            : m_playheadSec;
        setTimelineZoomAnchored(m_timelineZoom * factor, anchorSec, anchorU, keptDurSec);
        if (m_hwnd) InvalidateRect(m_hwnd, nullptr, FALSE);
        return;
    }
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
    bool needRepaint = false;
    if (m_hoveredCoachId != 0) {
        m_hoveredCoachId = 0;
        needRepaint = true;
    }
    if (m_hoveredHitId != Hit_None) {
        m_hoveredHitId = Hit_None;
        needRepaint = true;
    }
    if (m_hoveredStageAnnIdx != -1 || m_hoveredZoomFocusReticle ||
        m_hoveredWatermarkOnStage || m_hoveredWatermarkCorner) {
        m_hoveredStageAnnIdx = -1;
        m_hoveredStageAnnPart = StageAnnDragKind::None;
        m_hoveredZoomFocusReticle = false;
        m_hoveredWatermarkOnStage = false;
        m_hoveredWatermarkCorner = false;
        needRepaint = true;
    }
    if (m_hoverSkimming) {
        m_hoverSkimming = false;
        requestImmediatePreviewRefresh(true);
        return;
    }
    if (needRepaint && m_hwnd) {
        InvalidateRect(m_hwnd, nullptr, FALSE);
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
    if (m_hoveredCoachId != 0) {
        m_hoveredCoachId = 0;
        InvalidateRect(m_hwnd, nullptr, FALSE);
    }
    if (m_preparingSession) {
        for (auto it = m_hitZones.rbegin(); it != m_hitZones.rend(); ++it) {
            if (!pointInRectF(it->rect, lx, ly)) continue;
            if (it->id == Hit_MinimizeBtn) {
                if (m_hwnd) ShowWindow(m_hwnd, SW_MINIMIZE);
                return;
            }
            if (it->id == Hit_MaxRestoreBtn) {
                toggleMaximizeRestore();
                return;
            }
        }
        return;
    }

    const double keptDurSec = computeEffectiveDurationSec();

    for (auto it = m_hitZones.rbegin(); it != m_hitZones.rend(); ++it) {
        if (!pointInRectF(it->rect, lx, ly)) continue;

        const int id = it->id;

        // ── 导出模态窗热区处理（严格限定在模态窗打开态，严禁误吞 300..999 区间的抽屉控件 ID！） ──
        if (m_exportModalOpen && m_openDropdown != ExportDropdownKind::None &&
            id >= Hit_ModalDropdownItemBase && id < Hit_ModalDropdownItemBase + 32) {
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
                m_exportCompletedCard = false;
                m_editingExportName = false;
                m_openDropdown = ExportDropdownKind::None;
                InvalidateRect(m_hwnd, nullptr, FALSE);
            }
            return;
        }
        if (id == Hit_ModalResultCopyFileBtn) {
            copyLastExportedToClipboard();
            return;
        }
        if (id == Hit_ModalResultOpenPlayBtn) {
            if (!m_lastExportedFilePath.empty()) {
                tools3000::core::WinUtils::openFile(
                    tools3000::core::WinUtils::utf8ToWstring(m_lastExportedFilePath));
            }
            return;
        }
        if (id == Hit_ModalResultRevealBtn) {
            if (!m_lastExportedFilePath.empty()) {
                tools3000::core::WinUtils::openFolderAndSelectItem(
                    tools3000::core::WinUtils::utf8ToWstring(m_lastExportedFilePath));
            }
            return;
        }
        if (id == Hit_ModalResultExportMoreBtn) {
            m_exportCompletedCard = false;
            m_editingExportName = false;
            m_openDropdown = ExportDropdownKind::None;
            ensureNonCollidingExportStem();
            InvalidateRect(m_hwnd, nullptr, FALSE);
            return;
        }
        if (id == Hit_ModalResultFinishCloseBtn) {
            finishExportedSessionAndClose();
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
        if (id == Hit_ModalAutoCloseCheck) {
            m_editingExportName = false;
            m_openDropdown = ExportDropdownKind::None;
            m_autoCloseAfterExport = !m_autoCloseAfterExport;
            tools3000::core::ConfigManager::instance().set<bool>(
                "/recording/studioExport/autoClose", m_autoCloseAfterExport);
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
        if (id != Hit_WatermarkTextInput && !(id >= Hit_WatermarkTokenBase && id < Hit_WatermarkTokenBase + 4)) {
            m_editingWatermarkText = false;
        }

        // ── 顶栏与导航热区 ──
        if (id == Hit_MinimizeBtn) {
            if (m_hwnd) ShowWindow(m_hwnd, SW_MINIMIZE);
            return;
        }
        if (id == Hit_MaxRestoreBtn) {
            toggleMaximizeRestore();
            return;
        }
        if (id == Hit_CloseBtn) {
            if (m_exporting.load(std::memory_order_acquire)) {
                m_cancelExport.store(true, std::memory_order_release);
                joinExportWorkerIfDone(true);
                m_exporting.store(false, std::memory_order_release);
            }
            closeOrKeepBySessionState();
            return;
        }
        if (id == Hit_KeepOriginalBtn) {
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
            m_exportCompletedCard = false;
            m_editingExportName = false;
            m_openDropdown = ExportDropdownKind::None;
            ensureNonCollidingExportStem();
            InvalidateRect(m_hwnd, nullptr, FALSE);
            return;
        }
        if (id >= Hit_LeftRailTabBase && id < Hit_LeftRailTabBase + 5) {
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
        if (id == Hit_TransportJumpStart) {
            m_playing = false;
            m_hoverSkimming = false;
            m_playheadSec = 0.0;
            m_timelineViewStartSec = 0.0;
            requestImmediatePreviewRefresh(true);
            return;
        }
        if (id == Hit_TransportPrevFrame || id == Hit_TransportNextFrame) {
            m_playing = false;
            m_hoverSkimming = false;
            const double frameStep = (id == Hit_TransportNextFrame) ? (1.0 / 30.0) : (-1.0 / 30.0);
            m_playheadSec = std::clamp(m_playheadSec + frameStep, 0.0, keptDurSec);
            if (m_timelineZoom > 1.01f) {
                const double visSpan = visibleTimelineSpanSec(keptDurSec);
                if (m_playheadSec < m_timelineViewStartSec) {
                    m_timelineViewStartSec = m_playheadSec;
                } else if (m_playheadSec > m_timelineViewStartSec + visSpan) {
                    m_timelineViewStartSec = m_playheadSec - visSpan;
                }
                clampTimelineViewWindow(keptDurSec);
            }
            requestImmediatePreviewRefresh(true);
            return;
        }
        if (id == Hit_PlayPauseBtn) {
            m_hoverSkimming = false;
            m_playing = !m_playing;
            m_lastTickMs = GetTickCount();
            requestImmediatePreviewRefresh(true);
            return;
        }
        if (id == Hit_TimelineZoomFit) {
            if (m_timelineZoom > 1.02f) {
                setTimelineZoomAnchored(1.0f, m_playheadSec, 0.5f, keptDurSec);
            } else {
                setTimelineZoomAnchored(2.5f, m_playheadSec, 0.5f, keptDurSec);
            }
            InvalidateRect(m_hwnd, nullptr, FALSE);
            return;
        }
        if (id == Hit_TimelineZoomOut || id == Hit_TimelineZoomIn) {
            const float mul = (id == Hit_TimelineZoomIn) ? 1.45f : (1.0f / 1.45f);
            setTimelineZoomAnchored(m_timelineZoom * mul, m_playheadSec, 0.5f, keptDurSec);
            InvalidateRect(m_hwnd, nullptr, FALSE);
            return;
        }
        if (id == Hit_TimelineZoomSlider || id == Hit_TimelinePanBar) {
            m_activeSlider = (id == Hit_TimelineZoomSlider)
                ? SliderDragKind::TimelineZoom
                : SliderDragKind::TimelinePan;
            SetCapture(m_hwnd);
            applySliderAtX(m_activeSlider, lx);
            return;
        }
        if (id == Hit_TimelineTopBarScrub || id == Hit_TimelineRulerScrub) {
            m_playing = false;
            m_hoverSkimming = false;
            m_scrubbingTimeline = true;
            m_scrubOnLaneArea = true;
            SetCapture(m_hwnd);
            const float laneLeft = m_cutLaneRect.left;
            const float trackW = (std::max)(1.0f, m_cutLaneRect.right - laneLeft);
            m_playheadSec = mapTrackXToKeptSec(lx, laneLeft, trackW, keptDurSec);
            requestImmediatePreviewRefresh(true);
            return;
        }

        // ── 企业与品牌水印抽屉交互热区 ──
        if (id == Hit_WatermarkToggle) {
            m_config.watermark.enabled = !m_config.watermark.enabled;
            WatermarkRenderer::saveStudioWatermarkConfig(m_config.watermark);
            requestImmediatePreviewRefresh(true);
            return;
        }
        if (id >= Hit_WatermarkModeBase && id < Hit_WatermarkModeBase + 3) {
            const auto newMode = static_cast<WatermarkMode>(it->payload);
            m_config.watermark.mode = newMode;
            m_config.watermark.enabled = true;
            if (newMode == WatermarkMode::ImageLogo &&
                m_config.watermark.imagePathUtf8.empty() &&
                !m_recentLogoPaths.empty()) {
                m_config.watermark.imagePathUtf8 = m_recentLogoPaths.front();
            }
            if (newMode == WatermarkMode::TiledTraceability && m_config.watermark.opacity > 0.36f) {
                m_config.watermark.opacity = 0.16f;
            } else if (newMode != WatermarkMode::TiledTraceability && m_config.watermark.opacity < 0.30f) {
                m_config.watermark.opacity = 0.86f;
            }
            WatermarkRenderer::saveStudioWatermarkConfig(m_config.watermark);
            requestImmediatePreviewRefresh(true);
            return;
        }
        if (id == Hit_WatermarkPickImage) {
            browseWatermarkLogoImageAsync();
            return;
        }
        if (id >= Hit_WatermarkRecentBase && id < Hit_WatermarkRecentBase + 6) {
            const int rIdx = it->payload;
            if (rIdx >= 0 && rIdx < static_cast<int>(m_recentLogoPaths.size())) {
                m_config.watermark.imagePathUtf8 = m_recentLogoPaths[static_cast<size_t>(rIdx)];
                m_config.watermark.mode = WatermarkMode::ImageLogo;
                m_config.watermark.enabled = true;
                if (m_config.watermark.opacity < 0.30f) m_config.watermark.opacity = 0.88f;
                WatermarkRenderer::pushStudioRecentLogoPath(m_config.watermark.imagePathUtf8);
                WatermarkRenderer::saveStudioWatermarkConfig(m_config.watermark);
                m_recentLogoPaths = WatermarkRenderer::loadStudioRecentLogoPaths();
                m_recentLogoThumbsDirty = true;
                requestImmediatePreviewRefresh(true);
            }
            return;
        }
        if (id == Hit_WatermarkTextInput) {
            m_playing = false;
            m_editingWatermarkText = true;
            m_config.watermark.enabled = true;
            WatermarkRenderer::saveStudioWatermarkConfig(m_config.watermark);
            InvalidateRect(m_hwnd, nullptr, FALSE);
            return;
        }
        if (id >= Hit_WatermarkTokenBase && id < Hit_WatermarkTokenBase + 4) {
            const bool isZh = tools3000::core::NativeThemePalette::isUiLanguageChinese();
            static const char* kTokensZh[4] = { "{user}", "{host}", "{date} {time}", "内部资料 严禁外传" };
            static const char* kTokensEn[4] = { "{user}", "{host}", "{date} {time}", "CONFIDENTIAL" };
            const int k = it->payload;
            if (k >= 0 && k < 4) {
                std::string& tpl = m_config.watermark.textTemplate;
                auto eraseSubstrWithSep = [&](std::string_view needle) -> bool {
                    const size_t pos = tpl.find(needle);
                    if (pos == std::string::npos) return false;
                    tpl.erase(pos, needle.size());
                    // 清理残留的前后分隔符 " · "
                    while (true) {
                        const size_t dbl = tpl.find(" ·  · ");
                        if (dbl != std::string::npos) { tpl.replace(dbl, 8, " · "); continue; }
                        break;
                    }
                    if (tpl.rfind(" · ", 0) == 0) tpl.erase(0, 4);
                    if (tpl.size() >= 4 && tpl.compare(tpl.size() - 4, 4, " · ") == 0) tpl.erase(tpl.size() - 4);
                    while (!tpl.empty() && tpl.front() == ' ') tpl.erase(0, 1);
                    while (!tpl.empty() && tpl.back() == ' ') tpl.pop_back();
                    return true;
                };

                bool removed = false;
                if (k == 0) {
                    removed = eraseSubstrWithSep("{user}") || eraseSubstrWithSep("{USER}");
                } else if (k == 1) {
                    removed = eraseSubstrWithSep("{host}") || eraseSubstrWithSep("{HOST}");
                } else if (k == 2) {
                    const bool rDt = eraseSubstrWithSep("{date} {time}") || eraseSubstrWithSep("{datetime}");
                    const bool rD  = eraseSubstrWithSep("{date}");
                    const bool rT  = eraseSubstrWithSep("{time}");
                    removed = rDt || rD || rT;
                } else {
                    removed = eraseSubstrWithSep("内部资料 严禁外传") ||
                              eraseSubstrWithSep("严禁外传") ||
                              eraseSubstrWithSep("CONFIDENTIAL");
                }

                if (!removed) {
                    const char* tok = isZh ? kTokensZh[k] : kTokensEn[k];
                    if (!tpl.empty() && tpl.back() != ' ') {
                        tpl += " · ";
                    }
                    tpl += tok;
                    if (tpl.size() > 140) {
                        tpl.resize(140);
                    }
                }
                m_config.watermark.enabled = true;
                WatermarkRenderer::saveStudioWatermarkConfig(m_config.watermark);
                requestImmediatePreviewRefresh(true);
            }
            return;
        }
        if (id >= Hit_WatermarkAnchorBase && id < Hit_WatermarkAnchorBase + 5) {
            m_config.watermark.anchor = static_cast<WatermarkAnchor>(it->payload);
            m_config.watermark.enabled = true;
            WatermarkRenderer::saveStudioWatermarkConfig(m_config.watermark);
            requestImmediatePreviewRefresh(true);
            return;
        }
        if (id == Hit_WatermarkSliderScale || id == Hit_WatermarkSliderOpacity) {
            m_activeSlider = (id == Hit_WatermarkSliderScale)
                ? SliderDragKind::WatermarkScale
                : SliderDragKind::WatermarkOpacity;
            SetCapture(m_hwnd);
            applySliderAtX(m_activeSlider, lx);
            return;
        }

        // ── 视频标注卡片与属性热区（支持单击添加或按住直接拖拽至右侧舞台目标点放置） ──
        if (id >= Hit_AnnCardBase && id < Hit_AnnCardBase + 19) {
            m_pressedPresetCardIdx = it->payload;
            m_draggingPresetToStage = false;
            m_presetDragStartLx = lx;
            m_presetDragStartLy = ly;
            m_lastMouseLx = lx;
            m_lastMouseLy = ly;
            SetCapture(m_hwnd);
            InvalidateRect(m_hwnd, nullptr, FALSE);
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

        // ── 预览舞台点击：单点水印拖拽/缩放 -> 标注拖拽/缩放 -> 运镜段焦点拖拽 ──
        if (id == Hit_StageCanvas) {
            if (m_config.watermark.enabled && m_config.watermark.mode != WatermarkMode::TiledTraceability &&
                m_lastWatermarkStageRect.right > m_lastWatermarkStageRect.left + 4.0f &&
                m_lastBitmapDrawRect.right > m_lastBitmapDrawRect.left + 16.0f) {
                const float handleX = m_lastWatermarkStageRect.right + 4.0f;
                const float handleY = m_lastWatermarkStageRect.bottom + 4.0f;
                const D2D1_RECT_F wmHit = D2D1::RectF(
                    m_lastWatermarkStageRect.left - 6.0f, m_lastWatermarkStageRect.top - 6.0f,
                    m_lastWatermarkStageRect.right + 6.0f, m_lastWatermarkStageRect.bottom + 6.0f);
                if (std::hypot(lx - handleX, ly - handleY) <= 13.0f) {
                    m_playing = false;
                    m_activeTab = LeftRailTab::Watermark;
                    m_watermarkStageDragMode = 2;
                    m_wmDragStartLx = lx;
                    m_wmDragStartLy = ly;
                    m_wmOrigScale = m_config.watermark.scale;
                    SetCapture(m_hwnd);
                    SetCursor(resolveDesiredCursor(lx, ly));
                    InvalidateRect(m_hwnd, nullptr, FALSE);
                    return;
                }
                if (pointInRectF(wmHit, lx, ly)) {
                    m_playing = false;
                    m_activeTab = LeftRailTab::Watermark;
                    if (isDoubleClick && m_config.watermark.mode == WatermarkMode::CornerText) {
                        m_editingWatermarkText = true;
                        InvalidateRect(m_hwnd, nullptr, FALSE);
                        return;
                    }
                    const float bmpDrawW = (std::max)(1.0f, m_lastBitmapDrawRect.right - m_lastBitmapDrawRect.left);
                    const float bmpDrawH = (std::max)(1.0f, m_lastBitmapDrawRect.bottom - m_lastBitmapDrawRect.top);
                    const float wcx = (m_lastWatermarkStageRect.left + m_lastWatermarkStageRect.right) * 0.5f;
                    const float wcy = (m_lastWatermarkStageRect.top + m_lastWatermarkStageRect.bottom) * 0.5f;
                    m_wmOrigNormX = std::clamp((wcx - m_lastBitmapDrawRect.left) / bmpDrawW, 0.03f, 0.97f);
                    m_wmOrigNormY = std::clamp((wcy - m_lastBitmapDrawRect.top) / bmpDrawH, 0.03f, 0.97f);
                    m_config.watermark.customNormX = m_wmOrigNormX;
                    m_config.watermark.customNormY = m_wmOrigNormY;
                    m_config.watermark.anchor = WatermarkAnchor::CustomFree;
                    m_watermarkStageDragMode = 1;
                    m_wmDragStartLx = lx;
                    m_wmDragStartLy = ly;
                    SetCapture(m_hwnd);
                    SetCursor(resolveDesiredCursor(lx, ly));
                    InvalidateRect(m_hwnd, nullptr, FALSE);
                    return;
                }
            }

            float nx = 0.5f, ny = 0.5f;
            if (!mapStagePointToSourceNorm(lx, ly, nx, ny)) return;

            int hitAi = -1;
            StageAnnDragKind hitPart = StageAnnDragKind::None;
            if (hitTestStageAnnotation(lx, ly, hitAi, hitPart) && hitAi >= 0) {
                const auto& ann = m_timeline.annotations[static_cast<size_t>(hitAi)];
                m_playing = false;
                m_annDemoStartTick = 0;
                m_selectedAnnIdx = hitAi;
                m_selectedZoomIdx = -1;
                m_selectedClipIdx = -1;
                m_activeTab = LeftRailTab::Annotations;
                if (hitPart == StageAnnDragKind::MoveBody && isDoubleClick && isTextLikeAnnotation(ann.kind)) {
                    m_editingAnnotationText = true;
                } else {
                    pushUndoSnapshot();
                    m_stageAnnDrag = hitPart;
                    m_annDragStartNormX = nx;
                    m_annDragStartNormY = ny;
                    m_annOrigX0 = ann.x0; m_annOrigY0 = ann.y0;
                    m_annOrigX1 = ann.x1; m_annOrigY1 = ann.y1;
                    SetCapture(m_hwnd);
                }
                SetCursor(resolveDesiredCursor(lx, ly));
                requestImmediatePreviewRefresh(true);
                return;
            }

            if (m_selectedZoomIdx >= 0 && m_selectedZoomIdx < static_cast<int>(m_timeline.zoomSegments.size())) {
                pushUndoSnapshot();
                m_playing = false;
                m_hoverSkimming = false;
                auto& seg = m_timeline.zoomSegments[static_cast<size_t>(m_selectedZoomIdx)];
                const float srcW = static_cast<float>((std::max)(1, m_timeline.sourceWidth));
                const float srcH = static_cast<float>((std::max)(1, m_timeline.sourceHeight));
                float curSx = 0.0f, curSy = 0.0f;
                const bool nearExistingReticle =
                    mapSourceNormToStagePoint(seg.focusX / srcW, seg.focusY / srcH, curSx, curSy) &&
                    (std::hypot(lx - curSx, ly - curSy) <= 18.0f);
                if (!nearExistingReticle) {
                    seg.focusX = nx * srcW;
                    seg.focusY = ny * srcH;
                }
                seg.manualInserted = true;
                m_draggingZoomFocus = true;
                SetCapture(m_hwnd);
                SetCursor(resolveDesiredCursor(lx, ly));
                // 按下瞬间切换至 100% 全景底图，拖拽全程由 Direct2D 0ms 实时绘制光学取景框与 1:1 贴手准星
                updateCurrentComposedBitmap();
                RedrawWindow(m_hwnd, nullptr, nullptr, RDW_INVALIDATE | RDW_UPDATENOW);
            }
            return;
        }

        // ── 点击视频切片：选中该切片 + 定格绯红主指针 + 支持按住左键继续丝滑拖指针看画面！ ──
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

            const float laneLeft = m_cutLaneRect.left;
            const float w = (std::max)(1.0f, m_cutLaneRect.right - laneLeft);
            m_playheadSec = mapTrackXToKeptSec(lx, laneLeft, w, keptDurSec);
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
                ? static_cast<int64_t>(std::llround(mapTrackXToKeptSec(lx, m_zoomLaneRect.left, laneW, keptDurSec) * 1000.0))
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
                const int64_t steadyOffsetMs = (std::min<int64_t>)(420, (std::max<int64_t>)(80, (zseg.endMs - zseg.startMs) / 2));
                m_playheadSec = std::clamp(
                    StudioTimelineAnalyzer::mapSourceMsToKeptMsViaClips(m_videoClips, zseg.startMs + steadyOffsetMs) / 1000.0,
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
        if (id >= Hit_ClickStyleBase && id < Hit_ClickStyleBase + 4) {
            const auto chosen = static_cast<StudioClickEffectStyle>(id - Hit_ClickStyleBase);
            m_config.clickEffectStyle = chosen;
            m_config.showClickRipple = (chosen != StudioClickEffectStyle::TactileOnly);
            m_config.smoothCursorEnabled = true;
            persistStudioConfigToSettings(m_config);
            rebakeTimelinePreview();
            m_clickDemoStartTick = GetTickCount64();
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

    if (!m_exportModalOpen && ly <= 46.0f && m_hwnd) {
        if (isDoubleClick) {
            toggleMaximizeRestore();
            return;
        }
        m_draggingWindow = true;
        SetCapture(m_hwnd);
        GetCursorPos(&m_dragStartScreen);
        RECT wr{};
        GetWindowRect(m_hwnd, &wr);
        m_windowStartPos = {wr.left, wr.top};
    }
}

bool StudioPreviewWindow::hitTestStageAnnotation(
    float lx, float ly, int& outAnnIdx, StageAnnDragKind& outPart) const {
    outAnnIdx = -1;
    outPart = StageAnnDragKind::None;
    if (!pointInRectF(m_lastBitmapDrawRect, lx, ly)) {
        return false;
    }

    const int64_t curSrcMs = static_cast<int64_t>(std::llround(currentDisplayedSourceSec() * 1000.0));
    auto testSingleIndex = [&](int ai) -> bool {
        if (ai < 0 || ai >= static_cast<int>(m_timeline.annotations.size())) return false;
        const auto& ann = m_timeline.annotations[static_cast<size_t>(ai)];
        if (!ann.enabled || curSrcMs < ann.startMs || curSrcMs > ann.endMs) return false;

        float p0x = 0.0f, p0y = 0.0f, p1x = 0.0f, p1y = 0.0f;
        if (!mapSourceNormToStagePoint(ann.x0, ann.y0, p0x, p0y) ||
            !mapSourceNormToStagePoint(ann.x1, ann.y1, p1x, p1y)) {
            return false;
        }

        if (!isTextLikeAnnotation(ann.kind)) {
            const float endHx = isPointDirectedAnnotation(ann.kind) ? p1x : (std::max)(p0x, p1x) + 4.0f;
            const float endHy = isPointDirectedAnnotation(ann.kind) ? p1y : (std::max)(p0y, p1y) + 4.0f;
            if (std::hypot(lx - endHx, ly - endHy) <= 13.0f) {
                outAnnIdx = ai;
                outPart = StageAnnDragKind::DragEndPoint;
                return true;
            }
            const float startHx = isPointDirectedAnnotation(ann.kind) ? p0x : (std::min)(p0x, p1x) - 4.0f;
            const float startHy = isPointDirectedAnnotation(ann.kind) ? p0y : (std::min)(p0y, p1y) - 4.0f;
            if (std::hypot(lx - startHx, ly - startHy) <= 13.0f) {
                outAnnIdx = ai;
                outPart = StageAnnDragKind::DragStartPoint;
                return true;
            }
        }

        const D2D1_RECT_F hitBox = D2D1::RectF(
            (std::min)(p0x, p1x) - 22.0f, (std::min)(p0y, p1y) - 18.0f,
            (std::max)(p0x, p1x) + 22.0f, (std::max)(p0y, p1y) + 18.0f);
        if (pointInRectF(hitBox, lx, ly)) {
            outAnnIdx = ai;
            outPart = StageAnnDragKind::MoveBody;
            return true;
        }
        return false;
    };

    // 优先测试当前选中的标注控制柄，再从顶层向底层扫描其他可见标注
    if (m_selectedAnnIdx >= 0 && testSingleIndex(m_selectedAnnIdx)) {
        return true;
    }
    for (int ai = static_cast<int>(m_timeline.annotations.size()) - 1; ai >= 0; --ai) {
        if (ai == m_selectedAnnIdx) continue;
        if (testSingleIndex(ai)) return true;
    }
    return false;
}

HCURSOR StudioPreviewWindow::resolveDesiredCursor(float lx, float ly) const {
    (void)ly;
    // 1. 正在进行中的交互捕获状态优先锁定对应功能光标（即便滑出控件热区也稳态不跳）
    if (m_draggingWindow) {
        return LoadCursorW(nullptr, IDC_SIZEALL);
    }
    if (m_watermarkStageDragMode == 2) {
        return LoadCursorW(nullptr, IDC_SIZENWSE);
    }
    if (m_watermarkStageDragMode == 1) {
        return LoadCursorW(nullptr, IDC_SIZEALL);
    }
    if (m_draggingPresetToStage) {
        return LoadCursorW(nullptr, IDC_SIZEALL);
    }
    if (m_stageAnnDrag != StageAnnDragKind::None) {
        if (m_stageAnnDrag == StageAnnDragKind::MoveBody) {
            return LoadCursorW(nullptr, IDC_SIZEALL);
        }
        if (m_selectedAnnIdx >= 0 && m_selectedAnnIdx < static_cast<int>(m_timeline.annotations.size()) &&
            isPointDirectedAnnotation(m_timeline.annotations[static_cast<size_t>(m_selectedAnnIdx)].kind)) {
            return LoadCursorW(nullptr, IDC_CROSS);
        }
        return LoadCursorW(nullptr, IDC_SIZENWSE);
    }
    if (m_draggingZoomFocus) {
        return LoadCursorW(nullptr, IDC_SIZEALL);
    }
    if (m_activeSlider != SliderDragKind::None || m_scrubbingTimeline) {
        return LoadCursorW(nullptr, IDC_SIZEWE);
    }
    if (m_trackDragKind != TrackDragKind::None) {
        return (m_trackDragKind == TrackDragKind::ZoomMove || m_trackDragKind == TrackDragKind::AnnMove)
            ? LoadCursorW(nullptr, IDC_SIZEALL)
            : LoadCursorW(nullptr, IDC_SIZEWE);
    }

    // 2. 悬停预判态光标矩阵
    if (m_hoveredHitId == Hit_ModalNameInput || m_hoveredHitId == Hit_AnnTextInputBox ||
        m_hoveredHitId == Hit_WatermarkTextInput) {
        return LoadCursorW(nullptr, IDC_IBEAM);
    }
    if (m_hoveredHitId == Hit_SliderPadding || m_hoveredHitId == Hit_SliderInsetBorder ||
        m_hoveredHitId == Hit_SliderRadius || m_hoveredHitId == Hit_SliderShadow ||
        m_hoveredHitId == Hit_SliderBgBlur ||
        m_hoveredHitId == Hit_WatermarkSliderScale || m_hoveredHitId == Hit_WatermarkSliderOpacity ||
        m_hoveredHitId == Hit_TimelineZoomSlider || m_hoveredHitId == Hit_TimelinePanBar ||
        m_hoveredHitId == Hit_TimelineTopBarScrub || m_hoveredHitId == Hit_TimelineRulerScrub) {
        return LoadCursorW(nullptr, IDC_SIZEWE);
    }
    if ((m_hoveredHitId >= Hit_ZoomSegmentBase && m_hoveredHitId < Hit_VideoClipBase) ||
        m_hoveredHitId >= Hit_AnnSegmentBase) {
        for (const auto& hz : m_hitZones) {
            if (hz.id == m_hoveredHitId) {
                const float edgeTol = (std::min)(7.0f, (hz.rect.right - hz.rect.left) * 0.28f);
                return (lx <= hz.rect.left + edgeTol || lx >= hz.rect.right - edgeTol)
                    ? LoadCursorW(nullptr, IDC_SIZEWE)
                    : LoadCursorW(nullptr, IDC_SIZEALL);
            }
        }
    }
    if (m_hoveredHitId == Hit_StageCanvas) {
        if (m_hoveredWatermarkCorner) {
            return LoadCursorW(nullptr, IDC_SIZENWSE);
        }
        if (m_hoveredWatermarkOnStage) {
            return LoadCursorW(nullptr, IDC_SIZEALL);
        }
        if (m_hoveredStageAnnIdx >= 0 && m_hoveredStageAnnIdx < static_cast<int>(m_timeline.annotations.size())) {
            if (m_hoveredStageAnnPart == StageAnnDragKind::MoveBody) {
                return LoadCursorW(nullptr, IDC_SIZEALL);
            }
            const auto& ann = m_timeline.annotations[static_cast<size_t>(m_hoveredStageAnnIdx)];
            return isPointDirectedAnnotation(ann.kind)
                ? LoadCursorW(nullptr, IDC_CROSS)
                : LoadCursorW(nullptr, IDC_SIZENWSE);
        }
        if (m_selectedZoomIdx >= 0 && m_selectedZoomIdx < static_cast<int>(m_timeline.zoomSegments.size())) {
            return m_hoveredZoomFocusReticle
                ? LoadCursorW(nullptr, IDC_SIZEALL)
                : LoadCursorW(nullptr, IDC_CROSS);
        }
        return LoadCursorW(nullptr, IDC_ARROW);
    }
    if (m_hoveredHitId != Hit_None &&
        m_hoveredHitId != Hit_ModalBackdrop &&
        m_hoveredHitId != Hit_ModalCardBody) {
        return LoadCursorW(nullptr, IDC_HAND);
    }
    return LoadCursorW(nullptr, IDC_ARROW);
}

void StudioPreviewWindow::handleMouseMove(float lx, float ly, bool leftButtonDown) {
    m_lastMouseLx = lx;
    m_lastMouseLy = ly;

    if (!m_trackingMouseLeave && m_hwnd) {
        TRACKMOUSEEVENT tme{};
        tme.cbSize = sizeof(tme);
        tme.dwFlags = TME_LEAVE;
        tme.hwndTrack = m_hwnd;
        if (TrackMouseEvent(&tme)) {
            m_trackingMouseLeave = true;
        }
    }

    if (m_pressedPresetCardIdx >= 0 && leftButtonDown) {
        if (!m_draggingPresetToStage &&
            std::hypot(lx - m_presetDragStartLx, ly - m_presetDragStartLy) >= 5.0f) {
            m_draggingPresetToStage = true;
            m_hoveredCoachId = 0;
        }
        if (m_draggingPresetToStage) {
            InvalidateRect(m_hwnd, nullptr, FALSE);
            return;
        }
    }

    if (m_draggingWindow && leftButtonDown && m_hwnd) {
        POINT cur{};
        GetCursorPos(&cur);
        if (m_isMaximized) {
            if (std::hypot(static_cast<double>(cur.x - m_dragStartScreen.x),
                           static_cast<double>(cur.y - m_dragStartScreen.y)) >= 6.0) {
                const HMONITOR mon = MonitorFromWindow(m_hwnd, MONITOR_DEFAULTTONEAREST);
                const RECT work = tools3000::core::dpi::workArea(mon);
                int rw = m_restoreWindowRect.right - m_restoreWindowRect.left;
                int rh = m_restoreWindowRect.bottom - m_restoreWindowRect.top;
                if (rw < 640 || rh < 480) {
                    rw = tools3000::core::dpi::scaleMetric(m_logicalWidth, m_dpiScale);
                    rh = tools3000::core::dpi::scaleMetric(m_logicalHeight, m_dpiScale);
                }
                const double anchorRatio = std::clamp(
                    static_cast<double>(cur.x - work.left) / (std::max)(1.0, static_cast<double>(work.right - work.left)),
                    0.12, 0.88);
                const int newX = static_cast<int>(cur.x) - static_cast<int>(std::round(rw * anchorRatio));
                const int newY = (std::max<int>)(
                    static_cast<int>(work.top),
                    static_cast<int>(cur.y) - tools3000::core::dpi::scaleMetric(22, m_dpiScale));
                m_isMaximized = false;
                tools3000::core::ConfigManager::instance().set<bool>("/recording/studioWindowMaximized", false);
                m_dragStartScreen = cur;
                m_windowStartPos = {newX, newY};
                SetWindowPos(m_hwnd, nullptr, newX, newY, rw, rh, SWP_NOZORDER | SWP_NOACTIVATE | SWP_FRAMECHANGED);
                applyWindowRoundingForCurrentState(rw, rh);
            }
            return;
        }
        SetWindowPos(
            m_hwnd, nullptr,
            m_windowStartPos.x + (cur.x - m_dragStartScreen.x),
            m_windowStartPos.y + (cur.y - m_dragStartScreen.y),
            0, 0, SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
        return;
    }

    if (m_watermarkStageDragMode != 0 && leftButtonDown) {
        if (m_watermarkStageDragMode == 1) {
            const float bmpDrawW = (std::max)(1.0f, m_lastBitmapDrawRect.right - m_lastBitmapDrawRect.left);
            const float bmpDrawH = (std::max)(1.0f, m_lastBitmapDrawRect.bottom - m_lastBitmapDrawRect.top);
            m_config.watermark.customNormX = std::clamp(m_wmOrigNormX + (lx - m_wmDragStartLx) / bmpDrawW, 0.03f, 0.97f);
            m_config.watermark.customNormY = std::clamp(m_wmOrigNormY + (ly - m_wmDragStartLy) / bmpDrawH, 0.03f, 0.97f);
            m_config.watermark.anchor = WatermarkAnchor::CustomFree;
        } else if (m_watermarkStageDragMode == 2) {
            const float delta = ((lx - m_wmDragStartLx) + (ly - m_wmDragStartLy)) * 0.5f;
            m_config.watermark.scale = std::clamp(m_wmOrigScale * (1.0f + delta / 110.0f), 0.35f, 2.80f);
        }
        requestImmediatePreviewRefresh(false);
        return;
    }

    if (m_activeSlider != SliderDragKind::None && leftButtonDown) {
        applySliderAtX(m_activeSlider, lx);
        return;
    }

    if (m_draggingZoomFocus && leftButtonDown &&
        m_selectedZoomIdx >= 0 && m_selectedZoomIdx < static_cast<int>(m_timeline.zoomSegments.size())) {
        float nx = 0.5f, ny = 0.5f;
        if (mapStagePointToSourceNorm(lx, ly, nx, ny)) {
            auto& seg = m_timeline.zoomSegments[static_cast<size_t>(m_selectedZoomIdx)];
            seg.focusX = nx * static_cast<float>((std::max)(1, m_timeline.sourceWidth));
            seg.focusY = ny * static_cast<float>((std::max)(1, m_timeline.sourceHeight));
            seg.manualInserted = true;
            InvalidateRect(m_hwnd, nullptr, FALSE);
        }
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
                ann.x0 = std::clamp(m_annOrigX0 + dx, 0.0f, 1.0f - std::abs(w));
                ann.y0 = std::clamp(m_annOrigY0 + dy, 0.0f, 1.0f - std::abs(h));
                ann.x1 = std::clamp(ann.x0 + w, 0.0f, 1.0f);
                ann.y1 = std::clamp(ann.y0 + h, 0.0f, 1.0f);
            } else if (m_stageAnnDrag == StageAnnDragKind::DragStartPoint) {
                ann.x0 = std::clamp(nx, 0.02f, 0.98f);
                ann.y0 = std::clamp(ny, 0.02f, 0.98f);
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
    const double visSpanMs = (std::max)(100.0, visibleTimelineSpanSec(keptDurSec) * 1000.0);

    if (m_trackDragKind != TrackDragKind::None && leftButtonDown) {
        const float laneW = (std::max)(1.0f, m_zoomLaneRect.right - m_zoomLaneRect.left);
        const int64_t deltaKeptMs = static_cast<int64_t>(std::llround(((lx - m_trackDragStartX) / laneW) * visSpanMs));
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
                resequenceAutoStepBadges();
                m_playheadSec = std::clamp(previewKeptMs / 1000.0, 0.0, keptDurSec);
                requestImmediatePreviewRefresh(false);
            }
        }
        return;
    }

    // 按住左键在标尺或轨道区域拖拽主指针 (Active Scrubbing)
    if (m_scrubbingTimeline && leftButtonDown) {
        const float laneLeft = m_cutLaneRect.left;
        const float w = (std::max)(1.0f, m_cutLaneRect.right - laneLeft);
        m_playheadSec = mapTrackXToKeptSec(lx, laneLeft, w, keptDurSec);
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

    // 实时感知舞台可见水印角标、标注主体/缩放柄与运镜准星悬停态，驱动拖动光标与虚线感应框
    bool newHoverWm = false;
    bool newHoverWmCorner = false;
    int newStageAnnIdx = -1;
    StageAnnDragKind newStageAnnPart = StageAnnDragKind::None;
    bool newHoverZoomReticle = false;
    if (!m_exportModalOpen && newHover == Hit_StageCanvas) {
        if (m_config.watermark.enabled && m_config.watermark.mode != WatermarkMode::TiledTraceability &&
            m_lastWatermarkStageRect.right > m_lastWatermarkStageRect.left + 4.0f) {
            const float handleX = m_lastWatermarkStageRect.right + 4.0f;
            const float handleY = m_lastWatermarkStageRect.bottom + 4.0f;
            const D2D1_RECT_F wmHit = D2D1::RectF(
                m_lastWatermarkStageRect.left - 6.0f, m_lastWatermarkStageRect.top - 6.0f,
                m_lastWatermarkStageRect.right + 6.0f, m_lastWatermarkStageRect.bottom + 6.0f);
            if (std::hypot(lx - handleX, ly - handleY) <= 13.0f) {
                newHoverWm = true;
                newHoverWmCorner = true;
            } else if (pointInRectF(wmHit, lx, ly)) {
                newHoverWm = true;
            }
        }
        if (!newHoverWm) {
            hitTestStageAnnotation(lx, ly, newStageAnnIdx, newStageAnnPart);
            if (newStageAnnIdx < 0 && m_selectedZoomIdx >= 0 &&
                m_selectedZoomIdx < static_cast<int>(m_timeline.zoomSegments.size())) {
                const auto& zseg = m_timeline.zoomSegments[static_cast<size_t>(m_selectedZoomIdx)];
                const float srcW = static_cast<float>((std::max)(1, m_timeline.sourceWidth));
                const float srcH = static_cast<float>((std::max)(1, m_timeline.sourceHeight));
                float sx = 0.0f, sy = 0.0f;
                if (mapSourceNormToStagePoint(zseg.focusX / srcW, zseg.focusY / srcH, sx, sy)) {
                    newHoverZoomReticle = (std::hypot(lx - sx, ly - sy) <= 15.0f);
                }
            }
        }
    }
    const bool stageHoverChanged =
        (newHoverWm != m_hoveredWatermarkOnStage) ||
        (newHoverWmCorner != m_hoveredWatermarkCorner) ||
        (newStageAnnIdx != m_hoveredStageAnnIdx) ||
        (newStageAnnPart != m_hoveredStageAnnPart) ||
        (newHoverZoomReticle != m_hoveredZoomFocusReticle);
    m_hoveredWatermarkOnStage = newHoverWm;
    m_hoveredWatermarkCorner = newHoverWmCorner;
    m_hoveredStageAnnIdx = newStageAnnIdx;
    m_hoveredStageAnnPart = newStageAnnPart;
    m_hoveredZoomFocusReticle = newHoverZoomReticle;

    int newCoachHover = 0;
    if (!m_exportModalOpen && !leftButtonDown) {
        for (const auto& cz : m_coachHoverZones) {
            if (pointInRectF(cz.rect, lx, ly)) {
                newCoachHover = cz.id;
                break;
            }
        }
    }
    bool coachChanged = false;
    if (newCoachHover != m_hoveredCoachId) {
        const ULONGLONG now64 = GetTickCount64();
        if (newCoachHover != 0) {
            const bool warmChain = (m_hoveredCoachId != 0) ||
                                   (m_coachLastActiveTick > 0 && now64 >= m_coachLastActiveTick &&
                                    (now64 - m_coachLastActiveTick) <= 380);
            m_coachHoverEnterTick = warmChain ? ((now64 > 260) ? (now64 - 260) : 0) : now64;
        }
        m_hoveredCoachId = newCoachHover;
        coachChanged = true;
    }

    // ── FocuSee 级无按键悬停实时轨道掠览 (Live Hover Skimming) ──
    bool skimChanged = false;
    if (!m_exportModalOpen && !leftButtonDown && pointInRectF(m_allLanesHoverRect, lx, ly)) {
        const float laneW = (std::max)(1.0f, m_allLanesHoverRect.right - m_allLanesHoverRect.left);
        const double targetSkimSec = mapTrackXToKeptSec(lx, m_allLanesHoverRect.left, laneW, keptDurSec);
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

    if (newHover != m_hoveredHitId || newZoomHover != m_hoveredZoomIdx || coachChanged || stageHoverChanged) {
        m_hoveredHitId = newHover;
        m_hoveredZoomIdx = newZoomHover;
        InvalidateRect(m_hwnd, nullptr, FALSE);
    }
}

void StudioPreviewWindow::handleMouseUp() {
    if (m_pressedPresetCardIdx >= 0) {
        const int cardIdx = m_pressedPresetCardIdx;
        const bool wasDragging = m_draggingPresetToStage;
        m_pressedPresetCardIdx = -1;
        m_draggingPresetToStage = false;
        ReleaseCapture();

        if (wasDragging) {
            float dropNx = 0.5f, dropNy = 0.5f;
            if (pointInRectF(m_lastBitmapDrawRect, m_lastMouseLx, m_lastMouseLy) &&
                mapStagePointToSourceNorm(m_lastMouseLx, m_lastMouseLy, dropNx, dropNy)) {
                insertAnnotationCardPreset(cardIdx, &dropNx, &dropNy);
            } else {
                InvalidateRect(m_hwnd, nullptr, FALSE);
            }
        } else {
            insertAnnotationCardPreset(cardIdx, nullptr, nullptr);
        }
        SetCursor(resolveDesiredCursor(m_lastMouseLx, m_lastMouseLy));
        return;
    }

    const bool hadWmDrag = (m_watermarkStageDragMode != 0);
    const bool hadZoomFocusDrag = m_draggingZoomFocus;
    const bool hadCapture = m_scrubbingTimeline || m_draggingWindow ||
                            m_activeSlider != SliderDragKind::None ||
                            m_trackDragKind != TrackDragKind::None ||
                            m_stageAnnDrag != StageAnnDragKind::None ||
                            m_draggingZoomFocus || hadWmDrag;
    m_scrubbingTimeline = false;
    m_scrubOnLaneArea = false;
    m_draggingWindow = false;
    m_activeSlider = SliderDragKind::None;
    m_trackDragKind = TrackDragKind::None;
    m_trackDragIndex = -1;
    m_stageAnnDrag = StageAnnDragKind::None;
    m_draggingZoomFocus = false;
    m_watermarkStageDragMode = 0;
    if (hadWmDrag) {
        WatermarkRenderer::saveStudioWatermarkConfig(m_config.watermark);
    }
    if (hadZoomFocusDrag) {
        rebakeTimelinePreview();
    }
    if (hadCapture) {
        ReleaseCapture();
        resequenceAutoStepBadges();
        SetCursor(resolveDesiredCursor(m_lastMouseLx, m_lastMouseLy));
        requestImmediatePreviewRefresh(true);
    }
}

void StudioPreviewWindow::handleCharInput(wchar_t ch) {
    if (m_preparingSession || m_exporting.load(std::memory_order_acquire)) return;

    if (m_exportModalOpen && m_editingExportName) {
        if (ch == L'\b') {
            if (!m_exportFileNameStem.empty()) {
                m_exportFileNameStem.pop_back();
                m_userEditedExportStem = true;
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
            m_userEditedExportStem = true;
            InvalidateRect(m_hwnd, nullptr, FALSE);
        }
        return;
    }

    if (!m_exportModalOpen && m_editingWatermarkText) {
        std::wstring wtxt = tools3000::core::WinUtils::utf8ToWstring(m_config.watermark.textTemplate);
        if (ch == L'\b') {
            if (!wtxt.empty()) {
                wtxt.pop_back();
                m_config.watermark.textTemplate = tools3000::core::WinUtils::wstringToUtf8(wtxt);
                WatermarkRenderer::saveStudioWatermarkConfig(m_config.watermark);
                requestImmediatePreviewRefresh(true);
            }
            return;
        }
        if (ch == L'\r' || ch == L'\n') {
            m_editingWatermarkText = false;
            InvalidateRect(m_hwnd, nullptr, FALSE);
            return;
        }
        if (ch >= 32 && wtxt.size() < 110) {
            wtxt.push_back(ch);
            m_config.watermark.textTemplate = tools3000::core::WinUtils::wstringToUtf8(wtxt);
            m_config.watermark.enabled = true;
            WatermarkRenderer::saveStudioWatermarkConfig(m_config.watermark);
            requestImmediatePreviewRefresh(true);
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
                if (ann.kind == StudioAnnotationKind::StepBadge) {
                    ann.customStepText = !ann.text.empty();
                    if (!ann.customStepText) resequenceAutoStepBadges();
                }
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
            if (ann.kind == StudioAnnotationKind::StepBadge) {
                ann.customStepText = true;
            }
            requestImmediatePreviewRefresh(true);
        }
    }
}

void StudioPreviewWindow::handleKeyDown(WPARAM vk) {
    if (m_preparingSession || m_exporting.load(std::memory_order_acquire)) return;

    const bool ctrlDown = (GetAsyncKeyState(VK_CONTROL) & 0x8000) != 0;

    if (m_exportModalOpen) {
        if (m_exportCompletedCard) {
            if (vk == VK_ESCAPE) {
                m_exportModalOpen = false;
                m_exportCompletedCard = false;
                InvalidateRect(m_hwnd, nullptr, FALSE);
                return;
            }
            if (ctrlDown && vk == 'C') {
                copyLastExportedToClipboard();
                return;
            }
            if (vk == VK_RETURN) {
                finishExportedSessionAndClose();
                return;
            }
            return;
        }

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
                            if (isValidFileNameChar(wptr[i])) {
                                m_exportFileNameStem.push_back(wptr[i]);
                                m_userEditedExportStem = true;
                            }
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

    if (m_editingWatermarkText) {
        if (vk == VK_ESCAPE) {
            m_editingWatermarkText = false;
            InvalidateRect(m_hwnd, nullptr, FALSE);
            return;
        }
        if (ctrlDown && vk == 'V') {
            if (OpenClipboard(m_hwnd)) {
                if (HANDLE hData = GetClipboardData(CF_UNICODETEXT)) {
                    if (const auto* wptr = static_cast<const wchar_t*>(GlobalLock(hData))) {
                        std::wstring cur = tools3000::core::WinUtils::utf8ToWstring(m_config.watermark.textTemplate);
                        for (size_t i = 0; wptr[i] != L'\0' && cur.size() < 110; ++i) {
                            if (wptr[i] >= 32) cur.push_back(wptr[i]);
                        }
                        m_config.watermark.textTemplate = tools3000::core::WinUtils::wstringToUtf8(cur);
                        m_config.watermark.enabled = true;
                        WatermarkRenderer::saveStudioWatermarkConfig(m_config.watermark);
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
                        if (ann.kind == StudioAnnotationKind::StepBadge) {
                            ann.customStepText = !ann.text.empty();
                        }
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
        m_exportCompletedCard = false;
        m_editingExportName = false;
        m_openDropdown = ExportDropdownKind::None;
        ensureNonCollidingExportStem();
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

    if (vk == VK_HOME) {
        m_playing = false;
        m_hoverSkimming = false;
        m_playheadSec = 0.0;
        m_timelineViewStartSec = 0.0;
        requestImmediatePreviewRefresh(true);
        return;
    }

    if (vk == VK_LEFT || vk == VK_RIGHT) {
        m_playing = false;
        m_hoverSkimming = false;
        const double keptDurSec = computeEffectiveDurationSec();
        const bool shiftDown = (GetAsyncKeyState(VK_SHIFT) & 0x8000) != 0;
        const double step = (shiftDown ? 0.5 : (1.0 / 30.0)) * ((vk == VK_RIGHT) ? 1.0 : -1.0);
        m_playheadSec = std::clamp(m_playheadSec + step, 0.0, keptDurSec);
        if (m_timelineZoom > 1.01f) {
            const double visSpan = visibleTimelineSpanSec(keptDurSec);
            if (m_playheadSec < m_timelineViewStartSec) m_timelineViewStartSec = m_playheadSec;
            else if (m_playheadSec > m_timelineViewStartSec + visSpan) m_timelineViewStartSec = m_playheadSec - visSpan;
            clampTimelineViewWindow(keptDurSec);
        }
        requestImmediatePreviewRefresh(true);
        return;
    }

    if (ctrlDown && (vk == VK_OEM_PLUS || vk == VK_ADD || vk == VK_OEM_MINUS || vk == VK_SUBTRACT || vk == '0')) {
        const double keptDurSec = computeEffectiveDurationSec();
        if (vk == '0') {
            setTimelineZoomAnchored(1.0f, m_playheadSec, 0.5f, keptDurSec);
        } else {
            const bool zoomIn = (vk == VK_OEM_PLUS || vk == VK_ADD);
            setTimelineZoomAnchored(m_timelineZoom * (zoomIn ? 1.45f : (1.0f / 1.45f)),
                                    m_playheadSec, 0.5f, keptDurSec);
        }
        InvalidateRect(m_hwnd, nullptr, FALSE);
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
        closeOrKeepBySessionState();
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
        case WM_STUDIO_WATERMARK_IMG_PICKED:
            self->onWatermarkLogoPicked();
            return 0;
        case WM_NCCALCSIZE: {
            if (wParam) {
                if (IsZoomed(hwnd)) {
                    auto* params = reinterpret_cast<NCCALCSIZE_PARAMS*>(lParam);
                    const HMONITOR hMon = MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST);
                    MONITORINFO mi{};
                    mi.cbSize = sizeof(mi);
                    if (hMon && GetMonitorInfoW(hMon, &mi)) {
                        params->rgrc[0] = mi.rcWork;
                    }
                }
                return 0;
            }
            return DefWindowProcW(hwnd, msg, wParam, lParam);
        }
        case WM_GETMINMAXINFO: {
            auto* mmi = reinterpret_cast<MINMAXINFO*>(lParam);
            if (mmi) {
                const float s = (std::max)(1.0f, self->m_dpiScale);
                const HMONITOR mon = MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST);
                MONITORINFO mi{};
                mi.cbSize = sizeof(mi);
                if (mon && GetMonitorInfoW(mon, &mi)) {
                    const int workW = (std::max)(640, static_cast<int>(mi.rcWork.right - mi.rcWork.left));
                    const int workH = (std::max)(480, static_cast<int>(mi.rcWork.bottom - mi.rcWork.top));
                    mmi->ptMinTrackSize.x = (std::min<LONG>)(tools3000::core::dpi::scaleMetric(1020, s), static_cast<LONG>(workW * 0.92f));
                    mmi->ptMinTrackSize.y = (std::min<LONG>)(tools3000::core::dpi::scaleMetric(650, s), static_cast<LONG>(workH * 0.92f));
                    mmi->ptMaxPosition.x = mi.rcWork.left - mi.rcMonitor.left;
                    mmi->ptMaxPosition.y = mi.rcWork.top - mi.rcMonitor.top;
                    mmi->ptMaxSize.x = workW;
                    mmi->ptMaxSize.y = workH;
                } else {
                    mmi->ptMinTrackSize.x = tools3000::core::dpi::scaleMetric(1020, s);
                    mmi->ptMinTrackSize.y = tools3000::core::dpi::scaleMetric(650, s);
                }
            }
            return 0;
        }
        case WM_NCHITTEST: {
            RECT wr{};
            GetWindowRect(hwnd, &wr);
            const int sx = GET_X_LPARAM(lParam);
            const int sy = GET_Y_LPARAM(lParam);
            const float s = (std::max)(1.0f, self->m_dpiScale);
            const bool zoomed = (IsZoomed(hwnd) != FALSE);

            if (!zoomed && !self->m_exportModalOpen) {
                const int border = tools3000::core::dpi::scaleMetric(8, s);
                const int corner = tools3000::core::dpi::scaleMetric(16, s);

                const bool onLeft   = (sx >= wr.left && sx < wr.left + border);
                const bool onRight  = (sx <= wr.right && sx > wr.right - border);
                const bool onTop    = (sy >= wr.top && sy < wr.top + border);
                const bool onBottom = (sy <= wr.bottom && sy > wr.bottom - border);

                if ((sx < wr.left + corner && onTop) || (sy < wr.top + corner && onLeft)) return HTTOPLEFT;
                if ((sx > wr.right - corner && onTop) || (sy < wr.top + corner && onRight)) return HTTOPRIGHT;
                if ((sx < wr.left + corner && onBottom) || (sy > wr.bottom - corner && onLeft)) return HTBOTTOMLEFT;
                if ((sx > wr.right - corner && onBottom) || (sy > wr.bottom - corner && onRight)) return HTBOTTOMRIGHT;
                if (onLeft) return HTLEFT;
                if (onRight) return HTRIGHT;
                if (onTop) return HTTOP;
                if (onBottom) return HTBOTTOM;
            }

            if (!self->m_exportModalOpen) {
                POINT cliPt{sx, sy};
                ScreenToClient(hwnd, &cliPt);
                const float lx = cliPt.x / s;
                const float ly = cliPt.y / s;
                const float logicalW = (wr.right - wr.left) / s;
                // 顶部标题栏空白区返回原生 HTCAPTION：支持 Aero Snap 边缘贴靠、双击最大化与 Win+方向键分屏
                if (ly >= 8.0f && ly <= 45.0f && lx >= 8.0f && lx < (logicalW - 324.0f)) {
                    return HTCAPTION;
                }
            }
            return HTCLIENT;
        }
        case WM_ENTERSIZEMOVE:
            self->m_inInteractiveSizing = true;
            return 0;
        case WM_EXITSIZEMOVE: {
            self->m_inInteractiveSizing = false;
            if (!IsZoomed(hwnd)) {
                RECT wr{};
                if (GetWindowRect(hwnd, &wr)) {
                    const int pw = wr.right - wr.left;
                    const int ph = wr.bottom - wr.top;
                    if (pw >= 640 && ph >= 480) {
                        const float s = (std::max)(1.0f, self->m_dpiScale);
                        self->m_logicalWidth = std::clamp(static_cast<int>(std::round(pw / s)), 1020, 3840);
                        self->m_logicalHeight = std::clamp(static_cast<int>(std::round(ph / s)), 650, 2160);
                        self->m_restoreWindowRect = wr;
                        auto& cm = tools3000::core::ConfigManager::instance();
                        cm.set<int>("/recording/studioWindowWidth", self->m_logicalWidth);
                        cm.set<int>("/recording/studioWindowHeight", self->m_logicalHeight);
                    }
                    self->applyWindowRoundingForCurrentState(pw, ph);
                }
            }
            // 窗口拖拽缩放停稳瞬间，按最新舞台物理像素尺寸立即重烘 100% 视网膜原生超清画面
            self->updateCurrentComposedBitmap();
            InvalidateRect(hwnd, nullptr, FALSE);
            return 0;
        }
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
                self->applyWindowRoundingForCurrentState(w, h);
            }
            self->discardDeviceResources();
            self->updateCurrentComposedBitmap();
            InvalidateRect(hwnd, nullptr, FALSE);
            return 0;
        }
        case WM_SIZE: {
            if (wParam != SIZE_MINIMIZED) {
                const int pw = LOWORD(lParam);
                const int ph = HIWORD(lParam);
                const bool zoomed = (wParam == SIZE_MAXIMIZED || IsZoomed(hwnd) != FALSE);
                if (zoomed != self->m_isMaximized) {
                    self->m_isMaximized = zoomed;
                    tools3000::core::ConfigManager::instance().set<bool>("/recording/studioWindowMaximized", zoomed);
                }
                if (self->m_renderTarget && pw > 0 && ph > 0) {
                    self->m_renderTarget->Resize(D2D1::SizeU(static_cast<UINT32>(pw), static_cast<UINT32>(ph)));
                }
                if (!zoomed && pw >= 640 && ph >= 480) {
                    const float s = (std::max)(1.0f, self->m_dpiScale);
                    self->m_logicalWidth = std::clamp(static_cast<int>(std::round(pw / s)), 1020, 3840);
                    self->m_logicalHeight = std::clamp(static_cast<int>(std::round(ph / s)), 650, 2160);
                    GetWindowRect(hwnd, &self->m_restoreWindowRect);
                }
                self->m_stageViewportPhysW = (std::max)(320, pw - tools3000::core::dpi::scaleMetric(400, self->m_dpiScale));
                self->m_stageViewportPhysH = (std::max)(240, ph - tools3000::core::dpi::scaleMetric(256, self->m_dpiScale));
                self->applyWindowRoundingForCurrentState(pw, ph);
                if (!self->m_inInteractiveSizing && !self->m_previewDeckBgr.empty()) {
                    self->updateCurrentComposedBitmap();
                }
                InvalidateRect(hwnd, nullptr, FALSE);
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
        case WM_SETCURSOR: {
            if (LOWORD(lParam) == HTCLIENT) {
                SetCursor(self->resolveDesiredCursor(self->m_lastMouseLx, self->m_lastMouseLy));
                return TRUE;
            }
            break;
        }
        case WM_MOUSEMOVE: {
            const float s = (std::max)(1.0f, self->m_dpiScale);
            const float lx = GET_X_LPARAM(lParam) / s;
            const float ly = GET_Y_LPARAM(lParam) / s;
            self->handleMouseMove(lx, ly, (wParam & MK_LBUTTON) != 0);
            SetCursor(self->resolveDesiredCursor(lx, ly));
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
                self->closeOrKeepBySessionState();
            }
            return 0;
        default:
            break;
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

} // namespace tools3000::capture
