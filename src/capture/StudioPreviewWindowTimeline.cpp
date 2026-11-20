// ─────────────────────────────────────────────────────────────────────────────
// StudioPreviewWindowTimeline.cpp — 演播室动效教学卡、广播级游标标尺、一体化走带与胶片波形轨渲染子模块
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
            titleTxt = tools3000::core::trWCstrFor(isZh, L"在指针处拆分片段", L"Split Clip at Playhead");
            shortcutTxt = L"S";
            descTxt = tools3000::core::trWCstrFor(isZh, L"将当前视频从播放指针处切开一分为二。在需要剪掉的首尾位置各切一刀，即可分离出中间片段。", L"Splits the clip into two at the playhead. Cut at start and end to isolate unwanted sections.");
            break;
        case Hit_DeleteSelectedItem:
            titleTxt = tools3000::core::trWCstrFor(isZh, L"删除选中项并自动拼接", L"Ripple Delete");
            shortcutTxt = L"Del";
            descTxt = targetEnabled
                ? (tools3000::core::trWCstrFor(isZh, L"删除当前选中的视频片段、运镜或标注；删除片段后右侧画面自动向左无缝贴合，不留黑屏。", L"Removes the selected clip, zoom, or mark and snaps subsequent clips together seamlessly."))
                : (tools3000::core::trWCstrFor(isZh, L"先用「拆分(S)」切出片段并点击选中（或点选运镜/标注块），按 Del 即可删除并自动无缝拼接。", L"Split a clip with 'S' and click to select it first, then press Del to remove and snap."));
            break;
        case Hit_CycleClipSpeed:
            titleTxt = tools3000::core::trWCstrFor(isZh, L"片段局部变速", L"Clip Playback Speed");
            shortcutTxt = L"1.0x ~ 3.0x";
            descTxt = tools3000::core::trWCstrFor(isZh, L"为选中的单个片段循环切换 1.0x / 1.2x / 1.5x / 2.0x / 3.0x 倍速，轻松快进冗长的加载等待。", L"Cycles playback speed (1.0x to 3.0x) on the selected clip to fast-forward long waits.");
            break;
        case Hit_UndoBtn:
            titleTxt = tools3000::core::trWCstrFor(isZh, L"撤销上一步操作", L"Undo Last Action");
            shortcutTxt = L"Ctrl + Z";
            descTxt = tools3000::core::trWCstrFor(isZh, L"支持最多 40 步无损回退，随时撤回刚才的片段拆分、删除、变速、运镜或标注修改。", L"Reverts up to 40 steps of clip splits, deletions, speed changes, zooms, or annotations.");
            break;
        case Hit_RedoBtn:
            titleTxt = tools3000::core::trWCstrFor(isZh, L"重做已撤销操作", L"Redo Action");
            shortcutTxt = L"Ctrl + Y";
            descTxt = tools3000::core::trWCstrFor(isZh, L"恢复刚刚撤销的剪辑、变速、运镜或画面标注状态。", L"Restores the most recently undone timeline edit, camera zoom, or annotation.");
            break;
        case Hit_QuickOpenAnnotations:
            titleTxt = tools3000::core::trWCstrFor(isZh, L"画面重点标注", L"Timeline Annotations");
            shortcutTxt = tools3000::core::trWCstrFor(isZh, L"19 种样式", L"19 Styles");
            descTxt = tools3000::core::trWCstrFor(isZh, L"在指定时间段添加手绘重点圈、圆角框、箭头、步骤序号、聚光灯、局部放大镜或马赛克打码。", L"Places timed marker circles, boxes, arrows, step badges, spotlight, magnifier, or blur.");
            break;
        case Hit_AddZoomAtPlayhead:
            titleTxt = tools3000::core::trWCstrFor(isZh, L"插入镜头推近特写", L"Insert Camera Zoom");
            shortcutTxt = tools3000::core::trWCstrFor(isZh, L"可拖焦点", L"Drag Focus");
            descTxt = tools3000::core::trWCstrFor(isZh, L"在指针处新增一段平滑放大运镜；选中蓝色运镜块后，可在画面直接拖动十字准星调整聚焦位置。", L"Adds a smooth zoom-in segment at playhead; drag the stage crosshair to re-aim focus.");
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
            drawTextClip(tools3000::core::trWCstrFor(isZh, L"重点", L"Key"), fonts.captionFmt, bub, rgba(1.0f, 1.0f, 1.0f, std::min(1.0f, sc)),
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

namespace studio_detail {

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
                          tools3000::core::trWCstrFor(isZh, L"片段 %zu · %s", L"Clip %zu · %s"),
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
                                      tools3000::core::trWCstrFor(isZh, L"静音 %s", L"Silence %s"), silDurTxt.c_str());
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
                drawTxt(tools3000::core::trWCstrFor(isZh, L"无音频轨道 / 静音片段", L"Muted / No Audio Track"),
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

} // namespace studio_detail

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
                    const wchar_t* s1 = tools3000::core::trWCstrFor(isZh, L"拆分(S)", L"Split(S)");
                    const wchar_t* s2 = tools3000::core::trWCstrFor(isZh, L"标注", L"Mark");
                    const wchar_t* s3 = tools3000::core::trWCstrFor(isZh, L"运镜", L"Zoom");
                    rt->DrawText(s1, static_cast<UINT32>(wcslen(s1)), capFmt.Get(), D2D1::RectF(34.0f, 8.0f, 84.0f, 32.0f), br.Get());
                    rt->DrawText(L"1.0x", 4, monoFmt.Get(), D2D1::RectF(170.0f, 8.0f, 209.0f, 32.0f), br.Get());
                    rt->DrawText(s2, static_cast<UINT32>(wcslen(s2)), capFmt.Get(), D2D1::RectF(371.0f, 8.0f, 406.0f, 32.0f), br.Get());
                    rt->DrawText(s3, static_cast<UINT32>(wcslen(s3)), capFmt.Get(), D2D1::RectF(431.0f, 8.0f, 470.0f, 32.0f), br.Get());

                    br->SetColor(D2D1::ColorF(colPrimary.r, colPrimary.g, colPrimary.b, 0.35f));
                    const wchar_t* d1 = tools3000::core::trWCstrFor(isZh, L"删除", L"Delete");
                    const wchar_t* d2 = tools3000::core::trWCstrFor(isZh, L"撤销", L"Undo");
                    const wchar_t* d3 = tools3000::core::trWCstrFor(isZh, L"重做", L"Redo");
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
                    const wchar_t* zTxt = tools3000::core::trWCstrFor(isZh, L"全览", L"Fit");
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
                    const wchar_t* tLbl = tools3000::core::trWCstrFor(isZh, L"视频/音轨", L"Video/Aud");
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

} // namespace tools3000::capture
