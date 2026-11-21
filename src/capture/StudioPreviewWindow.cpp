// ─────────────────────────────────────────────────────────────────────────────
// StudioPreviewWindow.cpp — 演播室核心生命周期、会话装载、撤销重做、回放与共享辅助
// ─────────────────────────────────────────────────────────────────────────────

#include "capture/StudioPreviewWindowInternal.h"
#include "core/i18n/UiI18n.h"
#include "capture/StudioBackdropCatalog.h"
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

#include <opencv2/imgcodecs.hpp>
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

using Microsoft::WRL::ComPtr;
using namespace studio_detail;

namespace studio_detail {

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

D2D1_COLOR_F hexToD2DColor(const std::string& hex, float alpha) noexcept {
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

const wchar_t* formatLabel(StudioExportFormat f, bool isZh) noexcept {
    switch (f) {
        case StudioExportFormat::MP4_H264:      return tools3000::core::trWCstrFor(isZh, L"MP4 (通用视频 · 可放大)", L"MP4 (H.264 Video)");
        case StudioExportFormat::MP4_HEVC:      return tools3000::core::trWCstrFor(isZh, L"MP4 (HEVC 超清 · 可放大)", L"MP4 (HEVC Video)");
        case StudioExportFormat::GIF_WeChat:    return tools3000::core::trWCstrFor(isZh, L"GIF (微信表情 · 免点直播)", L"GIF (WeChat Sticker)");
        case StudioExportFormat::GIF_HD:        return tools3000::core::trWCstrFor(isZh, L"GIF (高清大图 · 可双击放大)", L"GIF (HD · Zoomable)");
        case StudioExportFormat::WebP_Animated: return tools3000::core::trWCstrFor(isZh, L"WebP (网页真彩动图)", L"WebP (Web Animated)");
        case StudioExportFormat::WebM_VP9:      return tools3000::core::trWCstrFor(isZh, L"WebM (VP9 视频)", L"WebM (VP9)");
        default:                                return L"MP4";
    }
}

const wchar_t* resolutionLabel(StudioExportResolution r, bool isZh) noexcept {
    switch (r) {
        case StudioExportResolution::Original: return tools3000::core::trWCstrFor(isZh, L"原始尺寸", L"Original");
        case StudioExportResolution::Res2160P: return L"4K (2160P)";
        case StudioExportResolution::Res1440P: return L"2K (1440P)";
        case StudioExportResolution::Res1080P: return L"1080P";
        case StudioExportResolution::Res720P:  return L"720P";
        case StudioExportResolution::Res640P:  return tools3000::core::trWCstrFor(isZh, L"640P (微信推荐)", L"640P (WeChat Fit)");
        case StudioExportResolution::Res480P:  return L"480P";
        default:                               return L"1080P";
    }
}

const wchar_t* qualityLabel(StudioExportQuality q, bool isZh) noexcept {
    switch (q) {
        case StudioExportQuality::High:    return tools3000::core::trWCstrFor(isZh, L"高画质", L"High Quality");
        case StudioExportQuality::Compact: return tools3000::core::trWCstrFor(isZh, L"轻量小体积", L"Compact Size");
        case StudioExportQuality::Recommended:
        default:                           return tools3000::core::trWCstrFor(isZh, L"推荐", L"Recommended");
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
        std::swprintf(buf, std::size(buf), tools3000::core::trWCstrFor(isZh, L"%d分%02d秒", L"%dm %02ds"), totalSecs / 60, totalSecs % 60);
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
    cm.set<double>("/recording/studioZoomPacing", static_cast<double>(cfg.zoomPacing));
    cm.set<double>("/recording/studioZoomSensitivity", static_cast<double>(cfg.autoZoomSensitivity));
    cm.set<double>("/recording/studioFocusSpotlightDim", static_cast<double>(cfg.focusSpotlightDim));
    cm.set<bool>("/recording/studioLockCameraHold", cfg.lockCameraWhileZoomed);
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
    const auto& resolvedBackdrop = StudioBackdropCatalog::resolvePreset(cfg.backgroundPresetKey, cfg.backgroundTheme);
    cm.set<std::string>("/recording/studioTheme", resolvedBackdrop.key);
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

void drawScissorsIcon(ID2D1RenderTarget* rt, ID2D1Brush* brush, float cx, float cy, ID2D1StrokeStyle* roundSt) {
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
    if (cardIndex < 0 || cardIndex >= 19) return tools3000::core::trWCstrFor(isZh, L"标注", L"Mark");
    return tools3000::core::trWCstrFor(isZh, kZh[cardIndex], kEn[cardIndex]);
}


} // namespace studio_detail

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

    // 1. 装载单一事实源内置 49 款高定背景（21 款原创高清壁纸 + 28 款零体积多光源渐变/纯色）
    const auto& catalog = StudioBackdropCatalog::presets();
    m_allWallpapers.reserve(catalog.size() + 8);
    for (const auto& preset : catalog) {
        WallpaperEntry entry{};
        entry.isBuiltin = true;
        entry.builtinTheme = preset.legacyTheme;
        entry.presetKey = preset.key;
        entry.categoryMask = preset.categoryMask;
        entry.displayName = tools3000::core::trWCstrFor(isZh, preset.nameZh, preset.nameEn);
        entry.category = 0;
        m_allWallpapers.push_back(std::move(entry));
    }

    // 2. 扫描用户本地扩展壁纸目录 (%LOCALAPPDATA%\Tools3000\wallpapers)，统一归入「自定义」分类
    std::set<std::wstring> seenNames;
    std::vector<std::filesystem::path> foundFiles;
    std::error_code ec;
    const std::wstring localAppData = queryKnownFolderPath(FOLDERID_LocalAppData);
    if (!localAppData.empty()) {
        const auto userWallDir = std::filesystem::path(localAppData) / L"Tools3000" / L"wallpapers";
        if (std::filesystem::is_directory(userWallDir, ec)) {
            for (const auto& de : std::filesystem::directory_iterator(userWallDir, ec)) {
                if (!de.is_regular_file(ec)) continue;
                const std::wstring ext = tools3000::core::WinUtils::toLower(de.path().extension().wstring());
                if (ext != L".jpg" && ext != L".jpeg" && ext != L".png" && ext != L".webp") continue;
                const std::wstring fnameLower = tools3000::core::WinUtils::toLower(de.path().filename().wstring());
                if (seenNames.insert(fnameLower).second) {
                    foundFiles.push_back(de.path());
                }
            }
        }
    }

    std::sort(foundFiles.begin(), foundFiles.end(), [](const auto& a, const auto& b) {
        return a.filename().wstring() < b.filename().wstring();
    });

    for (const auto& fp : foundFiles) {
        WallpaperEntry entry{};
        entry.isBuiltin = false;
        entry.filePathUtf8 = tools3000::core::WinUtils::wstringToUtf8(fp.wstring());
        entry.displayName = fp.stem().wstring();
        entry.category = 5; // 自定义
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
                customEntry.category = 5; // 自定义
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
        // 「全部」优先完整呈现官方 49 款高定内置背景（21 原创壁纸 + 28 渐变纯色，整 2 页排布）
        for (int i = 0; i < static_cast<int>(m_allWallpapers.size()); ++i) {
            if (m_allWallpapers[static_cast<size_t>(i)].isBuiltin) {
                m_filteredWallpaperIndices.push_back(i);
            }
        }
    } else if (m_wallpaperCategory >= 1 && m_wallpaperCategory <= 4) {
        // 1=沉稳, 2=轻盈, 3=风景, 4=渐变
        const uint32_t bit =
            (m_wallpaperCategory == 1) ? BackdropCat_Dark
          : (m_wallpaperCategory == 2) ? BackdropCat_Light
          : (m_wallpaperCategory == 3) ? BackdropCat_Scene
          : BackdropCat_Mesh;
        for (int i = 0; i < static_cast<int>(m_allWallpapers.size()); ++i) {
            const auto& w = m_allWallpapers[static_cast<size_t>(i)];
            if (w.isBuiltin && (w.categoryMask & bit) != 0u) {
                m_filteredWallpaperIndices.push_back(i);
            }
        }
    } else {
        // 5=自定义（+选图 导入与 %LOCALAPPDATA%\Tools3000\wallpapers 用户扩展图）
        for (int i = 0; i < static_cast<int>(m_allWallpapers.size()); ++i) {
            if (!m_allWallpapers[static_cast<size_t>(i)].isBuiltin) {
                m_filteredWallpaperIndices.push_back(i);
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

    MARGINS margins = {0, 0, 0, 1};
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
    m_closeConfirmModalOpen = false;
    m_closeAfterSaveExport = false;
    m_confirmSkipStudioNextTime = false;
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
    // 等待过渡层在支持环境下开启硬件级防截隐身（RDP / 旧版 Server 自动回退 WDA_NONE 保可见），并预先提交首帧 D2D 缓冲后再亮出窗口
    tools3000::core::WinUtils::applyCaptureExclusionIfSafe(m_hwnd, true);
    SetWindowPos(m_hwnd, nullptr, normX, normY, initWinW, initWinH, SWP_NOZORDER | SWP_NOACTIVATE);
    render();
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
        if (!StudioVideoCompositor::extractPreviewDeck(
                rawVideoPathUtf8, kStudioProxyMaxLongSide, kStudioProxyMaxFrames, deck, origW, origH, durSec) ||
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
    {
        const auto& resolvedBackdrop = StudioBackdropCatalog::resolvePreset(
            m_config.backgroundPresetKey, m_config.backgroundTheme);
        m_config.backgroundPresetKey = resolvedBackdrop.key;
        m_config.backgroundTheme = resolvedBackdrop.legacyTheme;
    }
    if (!m_config.autoZoomEnabled) {
        m_config.zoomMode = StudioZoomMode::None;
    } else if (m_config.zoomMode == StudioZoomMode::None) {
        m_config.zoomMode = StudioZoomMode::Zoom2D;
    }

    m_bitrateMbps = std::clamp(bitrateMbps, 2, 80);
    m_durationSec = (durSec > 0.05) ? durSec : (std::max)(0.1, m_timeline.durationMs / 1000.0);
    m_onFinished = std::move(onFinished);
    m_lastDecodedProxyIdx = static_cast<size_t>(-1);
    m_lastDecodedProxyBgr.release();
    const size_t proxyJpegBytes = StudioVideoCompositor::compressPreviewDeck(
        deck,
        m_previewDeckJpeg,
        m_visualDiffDeckGray,
        m_proxyFrameWidth,
        m_proxyFrameHeight,
        kStudioProxyMaxLongSide,
        kStudioProxyMaxFrames,
        kStudioProxyJpegQuality);
    deck.clear();
    deck.shrink_to_fit();
    if (m_previewDeckJpeg.empty()) {
        LOG_WARN("StudioPreviewWindow: failed to compress preview deck from {}, falling back to direct raw completion",
                 rawVideoPathUtf8);
        dismissPreparingOverlay();
        if (m_onFinished) {
            auto cb = std::move(m_onFinished);
            cb(rawVideoPathUtf8, m_durationSec, false);
        }
        return false;
    }
    m_filmstripDirty = true;
    m_origWidth = origW;
    m_origHeight = origH;

    if (m_timeline.zoomSegments.empty()) {
        m_timeline.zoomSegments = StudioTimelineAnalyzer::buildAutoZoomSegments(
            m_timeline, m_config.defaultZoomFactor, m_config.typingAutoZoom, m_config.autoZoomSensitivity);
    }
    StudioTimelineAnalyzer::refineZoomSegmentsWithVisualDiff(
        m_timeline, m_visualDiffDeckGray, m_durationSec, m_config.autoZoomSensitivity);

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
    m_closeConfirmModalOpen = false;
    m_closeAfterSaveExport = false;
    m_confirmSkipStudioNextTime = false;
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

    // 正式进入演播室编辑会话时恢复允许截图（WDA_NONE），并先完成高清帧读取器、时间轴与首帧画面预合成再亮出窗口
    tools3000::core::WinUtils::allowWindowCapture(m_hwnd);
    m_hiResReader.open(m_masterVideoPath, 3200);
    rebakeTimelinePreview();

    if (wasAlreadyShowingPrepare) {
        RECT cli{};
        GetClientRect(m_hwnd, &cli);
        const int curWinW = (std::max)(640, static_cast<int>(cli.right - cli.left));
        const int curWinH = (std::max)(480, static_cast<int>(cli.bottom - cli.top));
        m_stageViewportPhysW = (std::max)(640, curWinW - tools3000::core::dpi::scaleMetric(400, m_dpiScale));
        m_stageViewportPhysH = (std::max)(360, curWinH - tools3000::core::dpi::scaleMetric(256, m_dpiScale));
        updateCurrentComposedBitmap();
        render();
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

        updateCurrentComposedBitmap();
        SetWindowPos(m_hwnd, nullptr, normX, normY, initWinW, initWinH, SWP_NOZORDER | SWP_NOACTIVATE);
        render();

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

    SetTimer(m_hwnd, kPlaybackTimerId, kTimerIntervalMs, nullptr);
    const bool isZhReady = tools3000::core::NativeThemePalette::isUiLanguageChinese();
    SetWindowTextW(m_hwnd, isZhReady ? L"Tools3000 录屏演播室" : L"Tools3000 Studio");
    RedrawWindow(m_hwnd, nullptr, nullptr, RDW_INVALIDATE | RDW_UPDATENOW);

    LOG_INFO("StudioPreviewWindow opened: raw={}, deckFrames={}, proxyJpegKB={}, zoomSegments={}, wallpapers={}",
             m_rawVideoPath, m_previewDeckJpeg.size(), proxyJpegBytes / 1024,
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

const cv::Mat& StudioPreviewWindow::decodeProxyFrameAt(size_t index) {
    static const cv::Mat kEmptyMat;
    if (index >= m_previewDeckJpeg.size()) {
        return kEmptyMat;
    }
    if (m_lastDecodedProxyIdx == index && !m_lastDecodedProxyBgr.empty()) {
        return m_lastDecodedProxyBgr;
    }
    const auto& jpegBytes = m_previewDeckJpeg[index];
    if (jpegBytes.empty()) {
        return kEmptyMat;
    }
    cv::Mat decoded = cv::imdecode(jpegBytes, cv::IMREAD_COLOR);
    if (decoded.empty()) {
        return kEmptyMat;
    }
    m_lastDecodedProxyIdx = index;
    m_lastDecodedProxyBgr = std::move(decoded);
    return m_lastDecodedProxyBgr;
}

void StudioPreviewWindow::updateCurrentComposedBitmap() {
    if (m_previewDeckJpeg.empty()) return;

    const double srcSec = currentDisplayedSourceSec();
    const double norm = (m_durationSec > 1e-4)
        ? std::clamp(srcSec / m_durationSec, 0.0, 1.0)
        : 0.0;
    const size_t deckIdx = (std::min)(
        m_previewDeckJpeg.size() - 1,
        static_cast<size_t>(std::llround(norm * static_cast<double>(m_previewDeckJpeg.size() - 1))));

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
            eval.spotlightAlpha = lerpF(eval.spotlightAlpha, e1.spotlightAlpha);
            eval.spotlightCenterX = lerpF(eval.spotlightCenterX, e1.spotlightCenterX);
            eval.spotlightCenterY = lerpF(eval.spotlightCenterY, e1.spotlightCenterY);
            eval.spotlightHalfW = lerpF(eval.spotlightHalfW, e1.spotlightHalfW);
            eval.spotlightHalfH = lerpF(eval.spotlightHalfH, e1.spotlightHalfH);
        }
    }

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
            useNative = m_hiResReader.decodeFrameAt(srcSec, nativeFrame, /*cacheInLru=*/true);
        } else {
            const int proxyW = (m_proxyFrameWidth > 0) ? m_proxyFrameWidth : kStudioProxyMaxLongSide;
            const bool needHiResWhilePlaying =
                (m_stageViewportPhysW > proxyW + 24) ||
                (eval.camera.zoom > 1.08f) ||
                (m_hiResReader.nativeWidth() > proxyW);
            if (needHiResWhilePlaying) {
                // 顺序播放期间不写入多帧 LRU（仅刷新单帧缓存），杜绝播放几秒后内存膨胀，暂停后仍可 0ms 命中
                useNative = m_hiResReader.decodeFrameAt(srcSec, nativeFrame, /*cacheInLru=*/false);
            }
        }
    }
    m_lastComposedUsedProxy = (!useNative && m_hiResReader.isOpen()) || interactiveControlDrag;
    const cv::Mat& srcFrame = (useNative && !nativeFrame.empty())
        ? nativeFrame
        : decodeProxyFrameAt(deckIdx);
    if (srcFrame.empty()) return;

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
            ? std::clamp(m_config.paddingRatio, 0.0f, 0.22f)
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
                ? StudioVideoCompositor::renderPresetThumbnailBgra(entry.presetKey, entry.builtinTheme, 64, 64)
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
        if (!m_previewDeckJpeg.empty()) {
            D2D1_BITMAP_PROPERTIES bp = D2D1::BitmapProperties(
                D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_IGNORE),
                96.0f, 96.0f);
            const size_t totalDeck = m_previewDeckJpeg.size();
            const size_t stripCount = (std::min<size_t>)(32, totalDeck);
            m_filmstripBitmaps.reserve(stripCount);
            for (size_t si = 0; si < stripCount; ++si) {
                const size_t srcIdx = (stripCount <= 1)
                    ? 0
                    : ((si * (totalDeck - 1) + (stripCount - 1) / 2) / (stripCount - 1));
                const cv::Mat& srcBgr = decodeProxyFrameAt(srcIdx);
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
    m_closeConfirmModalOpen = false;
    m_closeAfterSaveExport = false;
    m_confirmSkipStudioNextTime = false;
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
    m_previewDeckJpeg.clear();
    m_previewDeckJpeg.shrink_to_fit();
    m_visualDiffDeckGray.clear();
    m_visualDiffDeckGray.shrink_to_fit();
    m_lastDecodedProxyBgr.release();
    m_lastDecodedProxyIdx = static_cast<size_t>(-1);
    m_proxyFrameWidth = 0;
    m_proxyFrameHeight = 0;
    m_evaluatedFrames.clear();
    m_evaluatedFrames.shrink_to_fit();
    m_currentComposedBgra.release();
    m_previewCompositor.clearCaches();
    StudioVideoCompositor::clearGlobalCaches();
    m_allWallpapers.clear();
    m_allWallpapers.shrink_to_fit();
    m_filteredWallpaperIndices.clear();
    m_filteredWallpaperIndices.shrink_to_fit();
    m_wallpapersDiscovered = false;
    m_wallpaperThumbsDirty = true;
    discardDeviceResources();
    CoFreeUnusedLibrariesEx(0, 0);
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

} // namespace tools3000::capture
