// ─────────────────────────────────────────────────────────────────────────────
// StudioPreviewWindowRender.cpp — 演播室就绪遮罩与主舞台/时间轴全量 D2D 渲染管线子模块
// ─────────────────────────────────────────────────────────────────────────────

#include "capture/StudioPreviewWindowInternal.h"
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
    const bool modalBlocked = m_exportModalOpen || m_closeConfirmModalOpen;
    const D2D1_RECT_F closeRc = D2D1::RectF(W - 40.0f, 9.0f, W - 10.0f, 37.0f);
    const D2D1_RECT_F maxRc   = D2D1::RectF(closeRc.left - 33.0f, 9.0f, closeRc.left - 3.0f, 37.0f);
    const D2D1_RECT_F minRc   = D2D1::RectF(maxRc.left - 33.0f, 9.0f, maxRc.left - 3.0f, 37.0f);
    const float hdrSepX       = minRc.left - 9.0f;
    const D2D1_RECT_F exportBtnRc = D2D1::RectF(hdrSepX - 102.0f, 8.0f, hdrSepX - 10.0f, 38.0f);
    const D2D1_RECT_F keepBtnRc   = D2D1::RectF(exportBtnRc.left - 90.0f, 8.0f, exportBtnRc.left - 8.0f, 38.0f);
    const D2D1_RECT_F rawModeBtnRc = D2D1::RectF(keepBtnRc.left - 98.0f, 8.0f, keepBtnRc.left - 8.0f, 38.0f);
    const bool showRawModeBtn = (rawModeBtnRc.left > metaPillRc.right + 10.0f);

    if (!exporting && !modalBlocked) {
        if (showRawModeBtn) {
            m_hitZones.push_back({Hit_ToggleRawModeBtn, rawModeBtnRc, -1});
        }
        m_hitZones.push_back({Hit_KeepOriginalBtn, keepBtnRc, -1});
        m_hitZones.push_back({Hit_OpenExportModalBtn, exportBtnRc, -1});
        m_hitZones.push_back({Hit_MinimizeBtn, minRc, -1});
        m_hitZones.push_back({Hit_MaxRestoreBtn, maxRc, -1});
    }
    m_hitZones.push_back({Hit_CloseBtn, closeRc, -1});

    // 快捷模式切换微晶胶囊：[原样模式]（一键开关背景修饰与自动运镜并记住设置）
    if (showRawModeBtn) {
        const bool rawActive = isRawOutputModeActive();
        const bool hovRaw = (m_hoveredHitId == Hit_ToggleRawModeBtn && !modalBlocked);
        const D2D1_COLOR_F rawDotCol = rawActive
            ? D2D1::ColorF(0.11f, 0.72f, 0.45f, 1.0f)
            : (dark ? D2D1::ColorF(0.54f, 0.62f, 0.74f, 0.85f) : D2D1::ColorF(0.42f, 0.50f, 0.62f, 0.85f));
        if (rawActive) {
            setBrush(dark ? D2D1::ColorF(0.08f, 0.34f, 0.24f, hovRaw ? 0.72f : 0.52f)
                          : D2D1::ColorF(0.11f, 0.72f, 0.45f, hovRaw ? 0.18f : 0.12f));
            m_renderTarget->FillRoundedRectangle(D2D1::RoundedRect(rawModeBtnRc, 7.5f, 7.5f), brush.Get());
            setBrush(D2D1::ColorF(0.11f, 0.72f, 0.45f, hovRaw ? 0.86f : 0.60f));
            m_renderTarget->DrawRoundedRectangle(D2D1::RoundedRect(rawModeBtnRc, 7.5f, 7.5f), brush.Get(), 1.05f);
        } else {
            setBrush(dark ? D2D1::ColorF(1.0f, 1.0f, 1.0f, hovRaw ? 0.09f : 0.045f)
                          : D2D1::ColorF(0.06f, 0.10f, 0.18f, hovRaw ? 0.075f : 0.038f));
            m_renderTarget->FillRoundedRectangle(D2D1::RoundedRect(rawModeBtnRc, 7.5f, 7.5f), brush.Get());
            setBrush(hovRaw ? colAccent : colBorder);
            m_renderTarget->DrawRoundedRectangle(D2D1::RoundedRect(rawModeBtnRc, 7.5f, 7.5f), brush.Get(), hovRaw ? 1.1f : 0.9f);
        }
        const float dotCy = (rawModeBtnRc.top + rawModeBtnRc.bottom) * 0.5f;
        setBrush(rawDotCol);
        if (rawActive) {
            m_renderTarget->FillEllipse(D2D1::Ellipse({rawModeBtnRc.left + 13.0f, dotCy}, 3.6f, 3.6f), brush.Get());
        } else {
            m_renderTarget->DrawEllipse(D2D1::Ellipse({rawModeBtnRc.left + 13.0f, dotCy}, 3.3f, 3.3f), brush.Get(), 1.35f);
        }
        drawTextAligned(isZh ? L"原样模式" : L"Raw Mode", m_captionFmt.Get(),
                        D2D1::RectF(rawModeBtnRc.left + 21.0f, rawModeBtnRc.top, rawModeBtnRc.right - 5.0f, rawModeBtnRc.bottom),
                        rawActive ? (dark ? D2D1::ColorF(0.55f, 0.96f, 0.76f, 1.0f) : D2D1::ColorF(0.05f, 0.45f, 0.28f, 1.0f))
                                  : (hovRaw ? colPrimary : colSecondary),
                        DWRITE_TEXT_ALIGNMENT_CENTER);
    }

    // 次级按钮：微晶白瓷/曜石立体键帽 [保留原片]
    {
        const bool hovKeep = (m_hoveredHitId == Hit_KeepOriginalBtn && !modalBlocked);
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
                        zseg.manualInserted ? zseg.targetZoom : m_config.defaultZoomFactor, 1.00f, 2.80f);
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
            const float effZoom = std::clamp(
                seg.manualInserted ? seg.targetZoom : m_config.defaultZoomFactor, 1.00f, 2.80f);
            wchar_t zLbl[32]{};
            if (effZoom <= 1.02f && m_config.focusSpotlightDim > 0.005f) {
                if (zw >= 74.0f) {
                    std::swprintf(zLbl, std::size(zLbl), isZh ? L"聚光 1.0x" : L"Focus 1.0x");
                } else {
                    std::swprintf(zLbl, std::size(zLbl), L"1.0x");
                }
            } else if (zw >= 74.0f) {
                std::swprintf(zLbl, std::size(zLbl), isZh ? L"运镜 %.2fx" : L"Zoom %.2fx", effZoom);
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

    // 视频切片接缝微晶刻度线（直观映射拆分点与下方运镜/标注轨的垂直对齐基准）
    if (m_videoClips.size() > 1 && keptDurSec > 1e-4) {
        int64_t seamAccMs = 0;
        for (size_t i = 0; i + 1 < m_videoClips.size(); ++i) {
            seamAccMs += m_videoClips[i].keptDurationMs();
            const float seamX = mapKeptSecToTrackX(static_cast<double>(seamAccMs) / 1000.0, laneLeft, trackW, keptDurSec);
            if (seamX <= laneLeft + 1.0f || seamX >= laneRight - 1.0f) continue;
            const bool seamSnapped = m_timelineSnapActive &&
                                     m_timelineSnapTargetKind == TimelineSnapTargetKind::ClipSplit &&
                                     std::abs(m_timelineSnapLineKeptMs - seamAccMs) <= 8;
            setBrush(seamSnapped
                ? D2D1::ColorF(0.98f, 0.72f, 0.16f, 0.96f)
                : (dark ? D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.14f) : D2D1::ColorF(0.12f, 0.20f, 0.34f, 0.14f)));
            m_renderTarget->DrawLine({seamX, m_cutLaneRect.bottom + 1.0f},
                                     {seamX, m_annLaneRect.bottom}, brush.Get(), seamSnapped ? 1.5f : 0.85f);
        }
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

    // 磁性吸附命中时的贯通三轨高定琥珀金对齐激光线 + 暗房吸附徽章 + 拖拽区间实时时间浮标
    if (m_timelineSnapActive && keptDurSec > 1e-4) {
        const float snapX = mapKeptSecToTrackX(static_cast<double>(m_timelineSnapLineKeptMs) / 1000.0, laneLeft, trackW, keptDurSec);
        if (snapX >= laneLeft - 1.5f && snapX <= laneRight + 1.5f) {
            const float topY = m_cutLaneRect.top - 4.0f;
            const float botY = m_annLaneRect.bottom + 4.0f;
            // 柔和琥珀金光学光晕
            setBrush(D2D1::ColorF(0.98f, 0.68f, 0.12f, dark ? 0.32f : 0.28f));
            m_renderTarget->DrawLine({snapX, topY}, {snapX, botY}, brush.Get(), 5.5f);
            // 1.5px 锐利琥珀金核线
            setBrush(D2D1::ColorF(0.99f, 0.76f, 0.20f, 0.98f));
            m_renderTarget->DrawLine({snapX, topY}, {snapX, botY}, brush.Get(), 1.5f);
            // 顶/底微晶圆点锚标
            m_renderTarget->FillEllipse(D2D1::Ellipse({snapX, topY}, 2.6f, 2.6f), brush.Get());
            m_renderTarget->FillEllipse(D2D1::Ellipse({snapX, botY}, 2.6f, 2.6f), brush.Get());

            const wchar_t* kindLbl = isZh ? L"对齐" : L"Snap";
            switch (m_timelineSnapTargetKind) {
                case TimelineSnapTargetKind::ClipSplit:    kindLbl = isZh ? L"吸附拆分点" : L"Snap Split"; break;
                case TimelineSnapTargetKind::Playhead:     kindLbl = isZh ? L"吸附指针"   : L"Snap Head";  break;
                case TimelineSnapTargetKind::ZoomBoundary: kindLbl = isZh ? L"吸附运镜"   : L"Snap Zoom";  break;
                case TimelineSnapTargetKind::AnnBoundary:  kindLbl = isZh ? L"吸附标注"   : L"Snap Mark";  break;
                default: break;
            }
            const std::wstring snapTimeTxt = formatCompactDurationBadge(static_cast<double>(m_timelineSnapLineKeptMs) / 1000.0, isZh);
            const std::wstring snapBadge = std::wstring(kindLbl) + L" · " + snapTimeTxt;
            float estW = 0.0f;
            for (wchar_t wc : snapBadge) estW += (wc > 0x7F) ? 11.4f : 6.4f;
            const float bw = std::clamp(estW + 16.0f, 76.0f, 180.0f);
            const float bl = std::clamp(snapX - bw * 0.5f, laneLeft, laneRight - bw);
            const D2D1_RECT_F sRc = D2D1::RectF(bl, tlCard.top + 38.0f, bl + bw, tlCard.top + 56.0f);
            setBrush(D2D1::ColorF(0.08f, 0.10f, 0.16f, 0.96f));
            m_renderTarget->FillRoundedRectangle(D2D1::RoundedRect(sRc, 5.0f, 5.0f), brush.Get());
            setBrush(D2D1::ColorF(0.98f, 0.74f, 0.18f, 0.90f));
            m_renderTarget->DrawRoundedRectangle(D2D1::RoundedRect(sRc, 5.0f, 5.0f), brush.Get(), 1.0f);
            drawTextAligned(snapBadge, m_captionFmt.Get(), sRc,
                            D2D1::ColorF(0.99f, 0.88f, 0.54f, 1.0f), DWRITE_TEXT_ALIGNMENT_CENTER);
        }
    }

    // 拖拽运镜/标注块期间实时展示暗房起止时间与时长胶囊（Alt 自由微调态同步提示）
    if (m_trackDragKind != TrackDragKind::None && keptDurSec > 1e-4 &&
        m_trackDragCurrentKeptEndMs > m_trackDragCurrentKeptStartMs) {
        const double stSec = static_cast<double>(m_trackDragCurrentKeptStartMs) / 1000.0;
        const double edSec = static_cast<double>(m_trackDragCurrentKeptEndMs) / 1000.0;
        const double durSec = (std::max)(0.1, edSec - stSec);
        const bool isZoomDrag = (m_trackDragKind == TrackDragKind::ZoomMove ||
                                 m_trackDragKind == TrackDragKind::ZoomLeft ||
                                 m_trackDragKind == TrackDragKind::ZoomRight);
        const bool altFree = (GetAsyncKeyState(VK_MENU) & 0x8000) != 0;
        wchar_t spanBuf[96]{};
        if (altFree) {
            std::swprintf(spanBuf, std::size(spanBuf),
                          isZh ? L"%.1fs - %.1fs (%.1fs · 自由微调)" : L"%.1fs - %.1fs (%.1fs · Free)",
                          stSec, edSec, durSec);
        } else {
            std::swprintf(spanBuf, std::size(spanBuf), L"%.1fs - %.1fs (%.1fs)", stSec, edSec, durSec);
        }
        const std::wstring spanTxt(spanBuf);
        float estW = 0.0f;
        for (wchar_t wc : spanTxt) estW += (wc > 0x7F) ? 11.4f : 6.4f;
        const float bw = std::clamp(estW + 18.0f, 100.0f, 230.0f);
        const float sx = mapKeptSecToTrackX(stSec, laneLeft, trackW, keptDurSec);
        const float ex = mapKeptSecToTrackX(edSec, laneLeft, trackW, keptDurSec);
        const float cx = (sx + ex) * 0.5f;
        const float bl = std::clamp(cx - bw * 0.5f, laneLeft, laneRight - bw);
        const float baseTop = isZoomDrag ? m_zoomLaneRect.top : m_annLaneRect.top;
        const D2D1_RECT_F dRc = D2D1::RectF(bl, baseTop - 24.0f, bl + bw, baseTop - 4.0f);
        setBrush(D2D1::ColorF(0.08f, 0.11f, 0.18f, 0.95f));
        m_renderTarget->FillRoundedRectangle(D2D1::RoundedRect(dRc, 5.0f, 5.0f), brush.Get());
        setBrush(m_timelineSnapActive
            ? D2D1::ColorF(0.98f, 0.74f, 0.18f, 0.85f)
            : D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.18f));
        m_renderTarget->DrawRoundedRectangle(D2D1::RoundedRect(dRc, 5.0f, 5.0f), brush.Get(), 0.95f);
        drawTextAligned(spanTxt, m_captionFmt.Get(), dRc,
                        D2D1::ColorF(0.96f, 0.98f, 1.0f, 0.98f), DWRITE_TEXT_ALIGNMENT_CENTER);
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
    } else if (m_closeConfirmModalOpen) {
        renderUnsavedCloseModalOverlay(W, H, dark, isZh, brush.Get());
    }

    if (m_renderTarget->EndDraw() == D2DERR_RECREATE_TARGET) {
        discardDeviceResources();
    }
}

} // namespace tools3000::capture
