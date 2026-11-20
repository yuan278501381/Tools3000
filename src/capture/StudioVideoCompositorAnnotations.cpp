// ─────────────────────────────────────────────────────────────────────────────
// StudioVideoCompositorAnnotations.cpp — 演播室时间轴矢量标注（D2D 气泡/箭头/聚焦/马赛克/放大镜）渲染子模块
// ─────────────────────────────────────────────────────────────────────────────

#include "capture/StudioVideoCompositorInternal.h"
#include "capture/ScreenRecorder.h"
#include "capture/StudioBackdropCatalog.h"
#include "core/config/ConfigManager.h"
#include "core/logger/Logger.h"
#include "core/utils/DWriteFontUtils.h"
#include "core/utils/UiThreadJoin.h"
#include "core/utils/WinUtils.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <objbase.h>
#include <d2d1.h>
#include <dwrite.h>
#include <wrl/client.h>

#include <opencv2/imgcodecs.hpp>
#include <opencv2/imgproc.hpp>
#include <webp/encode.h>
#include <webp/mux.h>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/avutil.h>
#include <libavutil/imgutils.h>
#include <libavutil/mathematics.h>
#include <libavutil/opt.h>
#include <libswscale/swscale.h>
}

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <unordered_map>

namespace tools3000::capture {

using Microsoft::WRL::ComPtr;
using namespace compositor_detail;

namespace {

void alphaBlendPremultipliedBgraTransformedOntoBgr(
    const cv::Mat& srcBgra,
    cv::Mat& dstBgr,
    int centerX,
    int centerY,
    float scale,
    float opacity) {
    if (srcBgra.empty() || srcBgra.type() != CV_8UC4 || dstBgr.empty() || dstBgr.type() != CV_8UC3) return;
    const float clampedOp = std::clamp(opacity, 0.0f, 1.0f);
    if (clampedOp <= 0.005f) return;

    cv::Mat activeBgra = srcBgra;
    const float clampedScale = std::clamp(scale, 0.25f, 2.2f);
    if (std::abs(clampedScale - 1.0f) > 0.012f) {
        const int sw = (std::max)(4, static_cast<int>(std::round(srcBgra.cols * clampedScale)));
        const int sh = (std::max)(4, static_cast<int>(std::round(srcBgra.rows * clampedScale)));
        cv::resize(srcBgra, activeBgra, cv::Size(sw, sh), 0.0, 0.0,
                   clampedScale < 1.0f ? cv::INTER_AREA : cv::INTER_LINEAR);
    }

    const int dstX = std::clamp(centerX - activeBgra.cols / 2, 2, (std::max)(2, dstBgr.cols - activeBgra.cols - 2));
    const int dstY = std::clamp(centerY - activeBgra.rows / 2, 2, (std::max)(2, dstBgr.rows - activeBgra.rows - 2));

    if (clampedOp >= 0.995f) {
        alphaBlendPremultipliedBgraOntoBgr(activeBgra, dstBgr, dstX, dstY);
        return;
    }

    const int x0 = (std::max)(0, dstX);
    const int y0 = (std::max)(0, dstY);
    const int x1 = (std::min)(dstBgr.cols, dstX + activeBgra.cols);
    const int y1 = (std::min)(dstBgr.rows, dstY + activeBgra.rows);
    if (x1 <= x0 || y1 <= y0) return;

    const uint32_t op256 = static_cast<uint32_t>(std::round(clampedOp * 256.0f));
    for (int y = y0; y < y1; ++y) {
        const uint8_t* sRow = activeBgra.ptr<uint8_t>(y - dstY) + (x0 - dstX) * 4;
        uint8_t* dRow = dstBgr.ptr<uint8_t>(y) + x0 * 3;
        for (int x = x0; x < x1; ++x) {
            const uint32_t effA = (static_cast<uint32_t>(sRow[3]) * op256 + 128u) >> 8;
            if (effA > 0) {
                const uint32_t sb = (static_cast<uint32_t>(sRow[0]) * op256 + 128u) >> 8;
                const uint32_t sg = (static_cast<uint32_t>(sRow[1]) * op256 + 128u) >> 8;
                const uint32_t sr = (static_cast<uint32_t>(sRow[2]) * op256 + 128u) >> 8;
                const uint32_t invA = 255u - (std::min<uint32_t>)(255u, effA);
                dRow[0] = static_cast<uint8_t>((std::min<uint32_t>)(255u, sb + (dRow[0] * invA + 127u) / 255u));
                dRow[1] = static_cast<uint8_t>((std::min<uint32_t>)(255u, sg + (dRow[1] * invA + 127u) / 255u));
                dRow[2] = static_cast<uint8_t>((std::min<uint32_t>)(255u, sr + (dRow[2] * invA + 127u) / 255u));
            }
            sRow += 4;
            dRow += 3;
        }
    }
}

template <typename DrawFn>
void blendRoiOverlayBgr(cv::Mat& baseBgr, const cv::Rect& dirtyBounds, double alpha, DrawFn&& fn) {
    if (baseBgr.empty() || alpha <= 0.01) return;
    const cv::Rect valid = dirtyBounds & cv::Rect(0, 0, baseBgr.cols, baseBgr.rows);
    if (valid.width <= 1 || valid.height <= 1) return;
    const double clampedAlpha = std::clamp(alpha, 0.0, 1.0);
    if (clampedAlpha >= 0.995) {
        cv::Mat directRoi = baseBgr(valid);
        fn(directRoi, -valid.x, -valid.y);
        return;
    }
    cv::Mat baseRoi = baseBgr(valid);
    cv::Mat overlayRoi = baseRoi.clone();
    fn(overlayRoi, -valid.x, -valid.y);
    cv::addWeighted(overlayRoi, clampedAlpha, baseRoi, 1.0 - clampedAlpha, 0.0, baseRoi);
}

// 线程局部可复用 Direct2D 预乘 BGRA 矢量画板：为圆弧、手绘重点圈、圆角框提供原生圆头笔帽与 8x8 硬件级平滑抗锯齿
struct ThreadLocalD2DSurface {
    ComPtr<ID2D1Factory> factory;
    ComPtr<ID2D1DCRenderTarget> dcRt;
    ComPtr<ID2D1StrokeStyle> roundStroke;
    HDC memDc = nullptr;
    HBITMAP dibBmp = nullptr;
    HGDIOBJ oldBmp = nullptr;
    void* bits = nullptr;
    int capW = 0;
    int capH = 0;

    ~ThreadLocalD2DSurface() {
        dcRt.Reset();
        roundStroke.Reset();
        factory.Reset();
        if (memDc) {
            if (oldBmp) SelectObject(memDc, oldBmp);
            if (dibBmp) DeleteObject(dibBmp);
            DeleteDC(memDc);
        }
    }

    bool ensureCapacity(int w, int h) {
        if (!factory) {
            if (FAILED(D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, factory.GetAddressOf())) || !factory) {
                return false;
            }
        }
        if (!roundStroke) {
            D2D1_STROKE_STYLE_PROPERTIES sp = D2D1::StrokeStyleProperties(
                D2D1_CAP_STYLE_ROUND, D2D1_CAP_STYLE_ROUND, D2D1_CAP_STYLE_ROUND,
                D2D1_LINE_JOIN_ROUND, 4.0f, D2D1_DASH_STYLE_SOLID, 0.0f);
            factory->CreateStrokeStyle(sp, nullptr, 0, roundStroke.GetAddressOf());
        }
        if (!dcRt) {
            D2D1_RENDER_TARGET_PROPERTIES props = D2D1::RenderTargetProperties(
                D2D1_RENDER_TARGET_TYPE_DEFAULT,
                D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED));
            if (FAILED(factory->CreateDCRenderTarget(&props, dcRt.GetAddressOf())) || !dcRt) {
                return false;
            }
        }
        if (!memDc) {
            memDc = CreateCompatibleDC(nullptr);
            if (!memDc) return false;
        }
        if (!dibBmp || w > capW || h > capH) {
            const int allocW = (std::max)(capW, (w + 63) & ~63);
            const int allocH = (std::max)(capH, (h + 63) & ~63);
            BITMAPINFO bmi{};
            bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
            bmi.bmiHeader.biWidth = allocW;
            bmi.bmiHeader.biHeight = -allocH; // Top-down
            bmi.bmiHeader.biPlanes = 1;
            bmi.bmiHeader.biBitCount = 32;
            bmi.bmiHeader.biCompression = BI_RGB;
            void* newBits = nullptr;
            HBITMAP newBmp = CreateDIBSection(memDc, &bmi, DIB_RGB_COLORS, &newBits, nullptr, 0);
            if (!newBmp || !newBits) {
                if (newBmp) DeleteObject(newBmp);
                return false;
            }
            HGDIOBJ prev = SelectObject(memDc, newBmp);
            if (!oldBmp) oldBmp = prev;
            if (dibBmp) DeleteObject(dibBmp);
            dibBmp = newBmp;
            bits = newBits;
            capW = allocW;
            capH = allocH;
        }
        return true;
    }
};

template <typename D2DDrawFn>
void renderD2DRoiVectorOntoBgr(cv::Mat& baseBgr, const cv::Rect& dirtyBounds, float masterOpacity, D2DDrawFn&& fn) {
    if (baseBgr.empty() || masterOpacity <= 0.01f) return;
    const cv::Rect valid = dirtyBounds & cv::Rect(0, 0, baseBgr.cols, baseBgr.rows);
    if (valid.width <= 2 || valid.height <= 2) return;

    thread_local ThreadLocalD2DSurface s_surf;
    if (!s_surf.ensureCapacity(valid.width, valid.height)) return;

    RECT bindRc{0, 0, valid.width, valid.height};
    if (FAILED(s_surf.dcRt->BindDC(s_surf.memDc, &bindRc))) return;

    s_surf.dcRt->BeginDraw();
    s_surf.dcRt->SetAntialiasMode(D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);
    s_surf.dcRt->Clear(D2D1::ColorF(0.0f, 0.0f, 0.0f, 0.0f));
    ComPtr<ID2D1SolidColorBrush> brush;
    if (SUCCEEDED(s_surf.dcRt->CreateSolidColorBrush(D2D1::ColorF(1, 1, 1, 1), brush.GetAddressOf())) && brush) {
        fn(s_surf.factory.Get(), s_surf.dcRt.Get(), brush.Get(), s_surf.roundStroke.Get(),
           -static_cast<float>(valid.x), -static_cast<float>(valid.y));
    }
    if (FAILED(s_surf.dcRt->EndDraw())) {
        s_surf.dcRt.Reset();
        return;
    }

    cv::Mat roiBgra(valid.height, valid.width, CV_8UC4, s_surf.bits, static_cast<size_t>(s_surf.capW) * 4);
    if (masterOpacity >= 0.995f) {
        alphaBlendPremultipliedBgraOntoBgr(roiBgra, baseBgr, valid.x, valid.y);
    } else {
        const uint32_t op256 = static_cast<uint32_t>(std::round(std::clamp(masterOpacity, 0.0f, 1.0f) * 256.0f));
        for (int y = 0; y < valid.height; ++y) {
            const uint8_t* sRow = roiBgra.ptr<uint8_t>(y);
            uint8_t* dRow = baseBgr.ptr<uint8_t>(valid.y + y) + valid.x * 3;
            for (int x = 0; x < valid.width; ++x) {
                const uint32_t effA = (static_cast<uint32_t>(sRow[3]) * op256 + 128u) >> 8;
                if (effA > 0) {
                    const uint32_t sb = (static_cast<uint32_t>(sRow[0]) * op256 + 128u) >> 8;
                    const uint32_t sg = (static_cast<uint32_t>(sRow[1]) * op256 + 128u) >> 8;
                    const uint32_t sr = (static_cast<uint32_t>(sRow[2]) * op256 + 128u) >> 8;
                    const uint32_t invA = 255u - (std::min<uint32_t>)(255u, effA);
                    dRow[0] = static_cast<uint8_t>((std::min<uint32_t>)(255u, sb + (dRow[0] * invA + 127u) / 255u));
                    dRow[1] = static_cast<uint8_t>((std::min<uint32_t>)(255u, sg + (dRow[1] * invA + 127u) / 255u));
                    dRow[2] = static_cast<uint8_t>((std::min<uint32_t>)(255u, sr + (dRow[2] * invA + 127u) / 255u));
                }
                sRow += 4;
                dRow += 3;
            }
        }
    }
}

void drawStyledTextCalloutBgr(
    cv::Mat& innerBgr,
    int x, int y,
    StudioAnnotationKind kind,
    const std::string& textUtf8,
    const cv::Scalar& accentBgr,
    float scale = 1.0f,
    float opacity = 1.0f,
    int* outBoxRadius = nullptr) {
    if (textUtf8.empty() || innerBgr.empty()) return;

    struct CalloutBitmapCacheEntry {
        StudioAnnotationKind kind{};
        std::string text;
        uint32_t bgrPacked = 0;
        int refRows = 0;
        cv::Mat premulBgra;
    };
    thread_local std::vector<CalloutBitmapCacheEntry> s_calloutCache;
    thread_local ComPtr<ID2D1Factory> s_d2dFactory;
    thread_local ComPtr<IDWriteFactory> s_dwFactory;

    const uint32_t packedColor =
        (static_cast<uint32_t>(accentBgr[0]) & 0xFFu) |
        ((static_cast<uint32_t>(accentBgr[1]) & 0xFFu) << 8) |
        ((static_cast<uint32_t>(accentBgr[2]) & 0xFFu) << 16);

    for (const auto& entry : s_calloutCache) {
        if (entry.kind == kind && entry.bgrPacked == packedColor &&
            entry.refRows == innerBgr.rows && entry.text == textUtf8 && !entry.premulBgra.empty()) {
            if (outBoxRadius) {
                *outBoxRadius = (std::max)(entry.premulBgra.cols, entry.premulBgra.rows) / 2;
            }
            alphaBlendPremultipliedBgraTransformedOntoBgr(entry.premulBgra, innerBgr, x, y, scale, opacity);
            return;
        }
    }

    const std::wstring wide = tools3000::core::WinUtils::utf8ToWstring(textUtf8);
    if (wide.empty()) return;

    if (!s_d2dFactory && FAILED(D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, s_d2dFactory.GetAddressOf()))) {
        return;
    }
    if (!s_dwFactory && FAILED(DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory),
                                                   reinterpret_cast<IUnknown**>(s_dwFactory.GetAddressOf())))) {
        return;
    }
    ComPtr<ID2D1Factory> d2dFactory = s_d2dFactory;
    ComPtr<IDWriteFactory> dwFactory = s_dwFactory;

    const float fontSize = std::clamp(static_cast<float>(innerBgr.rows) *
        (kind == StudioAnnotationKind::TextPlain ? 0.032f : 0.026f), 14.0f, 34.0f);
    const wchar_t* fontFamily = tools3000::core::resolveInstalledUiFontFamily(dwFactory.Get());
    ComPtr<IDWriteTextFormat> fmt;
    if (FAILED(dwFactory->CreateTextFormat(
            fontFamily, nullptr, DWRITE_FONT_WEIGHT_BOLD,
            DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, fontSize, L"zh-CN", fmt.GetAddressOf())) || !fmt) {
        return;
    }
    fmt->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
    fmt->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);

    ComPtr<IDWriteTextLayout> layout;
    if (FAILED(dwFactory->CreateTextLayout(
            wide.c_str(), static_cast<UINT32>(wide.size()), fmt.Get(),
            static_cast<float>(innerBgr.cols) * 0.72f, static_cast<float>(innerBgr.rows) * 0.28f, layout.GetAddressOf()))) {
        return;
    }
    DWRITE_TEXT_METRICS m{};
    if (FAILED(layout->GetMetrics(&m))) return;

    const bool isBadge = (kind == StudioAnnotationKind::StepBadge);
    const bool isBubble = (kind == StudioAnnotationKind::TextBubble);
    const float tailH = isBubble ? std::clamp(fontSize * 0.42f, 7.0f, 13.0f) : 0.0f;

    int boxW = std::clamp(static_cast<int>(std::ceil(m.widthIncludingTrailingWhitespace + 30.0f)), 28, innerBgr.cols - 4);
    int bodyH = std::clamp(static_cast<int>(std::ceil(m.height + 16.0f)), 24, innerBgr.rows / 2);
    if (isBadge) {
        const int d = (std::max)(boxW - 10, bodyH + 4);
        boxW = d;
        bodyH = d;
    }
    const int totalH = (std::min)(innerBgr.rows - 4, bodyH + static_cast<int>(std::ceil(tailH)) + 4);

    layout.Reset();
    if (FAILED(dwFactory->CreateTextLayout(
            wide.c_str(), static_cast<UINT32>(wide.size()), fmt.Get(),
            static_cast<float>(boxW - 4), static_cast<float>(bodyH - 4), layout.GetAddressOf()))) {
        return;
    }

    BITMAPINFO bmi{};
    bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bmi.bmiHeader.biWidth = boxW;
    bmi.bmiHeader.biHeight = -totalH;
    bmi.bmiHeader.biPlanes = 1;
    bmi.bmiHeader.biBitCount = 32;
    bmi.bmiHeader.biCompression = BI_RGB;

    void* bits = nullptr;
    HDC memDc = CreateCompatibleDC(nullptr);
    if (!memDc) return;
    HBITMAP hBmp = CreateDIBSection(memDc, &bmi, DIB_RGB_COLORS, &bits, nullptr, 0);
    if (!hBmp || !bits) {
        if (hBmp) DeleteObject(hBmp);
        DeleteDC(memDc);
        return;
    }
    HGDIOBJ oldBmp = SelectObject(memDc, hBmp);
    std::memset(bits, 0, static_cast<size_t>(boxW) * static_cast<size_t>(totalH) * 4);

    D2D1_RENDER_TARGET_PROPERTIES props = D2D1::RenderTargetProperties(
        D2D1_RENDER_TARGET_TYPE_DEFAULT,
        D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED));
    ComPtr<ID2D1DCRenderTarget> rt;
    if (SUCCEEDED(d2dFactory->CreateDCRenderTarget(&props, rt.GetAddressOf())) && rt) {
        RECT bindRc{0, 0, boxW, totalH};
        if (SUCCEEDED(rt->BindDC(memDc, &bindRc))) {
            rt->BeginDraw();
            rt->SetTextAntialiasMode(D2D1_TEXT_ANTIALIAS_MODE_GRAYSCALE);
            rt->Clear(D2D1::ColorF(0, 0, 0, 0));

            const float ar = static_cast<float>(accentBgr[2] / 255.0);
            const float ag = static_cast<float>(accentBgr[1] / 255.0);
            const float ab = static_cast<float>(accentBgr[0] / 255.0);
            const D2D1_COLOR_F colAcc = D2D1::ColorF(ar, ag, ab, 0.96f);

            ComPtr<ID2D1SolidColorBrush> bBg, bBorder, bText, bShadow;
            rt->CreateSolidColorBrush(colAcc, bBg.GetAddressOf());
            rt->CreateSolidColorBrush(D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.96f), bBorder.GetAddressOf());
            rt->CreateSolidColorBrush(D2D1::ColorF(1.0f, 1.0f, 1.0f, 1.0f), bText.GetAddressOf());
            rt->CreateSolidColorBrush(D2D1::ColorF(0.04f, 0.06f, 0.10f, 0.86f), bShadow.GetAddressOf());

            const D2D1_RECT_F bodyRc = D2D1::RectF(2.0f, 2.0f, static_cast<float>(boxW) - 2.0f, static_cast<float>(bodyH));
            const float r = isBadge ? (bodyRc.bottom - bodyRc.top) * 0.5f
                                    : (std::min)((bodyRc.bottom - bodyRc.top) * 0.42f, 12.0f);
            const D2D1_ROUNDED_RECT rr = D2D1::RoundedRect(bodyRc, r, r);

            if (kind == StudioAnnotationKind::TextPlain) {
                if (bShadow) {
                    rt->DrawTextLayout(D2D1::Point2F(3.5f, 3.5f), layout.Get(), bShadow.Get());
                }
                if (bBg) {
                    rt->DrawTextLayout(D2D1::Point2F(2.0f, 2.0f), layout.Get(), bBg.Get());
                }
            } else if (kind == StudioAnnotationKind::StepBadge) {
                const float cx = (bodyRc.left + bodyRc.right) * 0.5f;
                const float cy = (bodyRc.top + bodyRc.bottom) * 0.5f;
                const float rad = (std::min)(bodyRc.right - bodyRc.left, bodyRc.bottom - bodyRc.top) * 0.48f;
                if (bBg) rt->FillEllipse(D2D1::Ellipse({cx, cy}, rad, rad), bBg.Get());
                if (bBorder) rt->DrawEllipse(D2D1::Ellipse({cx, cy}, rad, rad), bBorder.Get(), 2.0f);
                if (bText) rt->DrawTextLayout(D2D1::Point2F(2.0f, 1.0f), layout.Get(), bText.Get());
            } else if (kind == StudioAnnotationKind::TextBubble) {
                if (bBg) {
                    rt->FillRoundedRectangle(rr, bBg.Get());
                    ComPtr<ID2D1PathGeometry> tailGeo;
                    if (SUCCEEDED(d2dFactory->CreatePathGeometry(tailGeo.GetAddressOf())) && tailGeo) {
                        ComPtr<ID2D1GeometrySink> sink;
                        if (SUCCEEDED(tailGeo->Open(sink.GetAddressOf())) && sink) {
                            const float tx = (bodyRc.left + bodyRc.right) * 0.5f;
                            sink->BeginFigure({tx - tailH * 0.65f, bodyRc.bottom - 1.5f}, D2D1_FIGURE_BEGIN_FILLED);
                            sink->AddLine({tx + tailH * 0.65f, bodyRc.bottom - 1.5f});
                            sink->AddLine({tx - tailH * 0.15f, bodyRc.bottom + tailH});
                            sink->EndFigure(D2D1_FIGURE_END_CLOSED);
                            sink->Close();
                            rt->FillGeometry(tailGeo.Get(), bBg.Get());
                        }
                    }
                }
                if (bText) rt->DrawTextLayout(D2D1::Point2F(2.0f, 1.0f), layout.Get(), bText.Get());
            } else if (kind == StudioAnnotationKind::TextDashed) {
                ComPtr<ID2D1SolidColorBrush> glassBg;
                rt->CreateSolidColorBrush(D2D1::ColorF(0.12f, 0.15f, 0.22f, 0.84f), glassBg.GetAddressOf());
                if (glassBg) rt->FillRoundedRectangle(rr, glassBg.Get());
                D2D1_STROKE_STYLE_PROPERTIES sp = D2D1::StrokeStyleProperties();
                sp.dashStyle = D2D1_DASH_STYLE_DASH;
                sp.dashCap = D2D1_CAP_STYLE_ROUND;
                ComPtr<ID2D1StrokeStyle> dashStyle;
                d2dFactory->CreateStrokeStyle(sp, nullptr, 0, dashStyle.GetAddressOf());
                if (bBg) rt->DrawRoundedRectangle(rr, bBg.Get(), 2.0f, dashStyle.Get());
                if (bText) rt->DrawTextLayout(D2D1::Point2F(2.0f, 1.0f), layout.Get(), bText.Get());
            } else if (kind == StudioAnnotationKind::TextNeon) {
                if (bShadow) rt->FillRoundedRectangle(rr, bShadow.Get());
                ComPtr<ID2D1SolidColorBrush> glowB;
                rt->CreateSolidColorBrush(D2D1::ColorF(ar, ag, ab, 0.36f), glowB.GetAddressOf());
                if (glowB) rt->DrawRoundedRectangle(rr, glowB.Get(), 5.0f);
                if (bBg) rt->DrawRoundedRectangle(rr, bBg.Get(), 2.0f);
                if (bText) rt->DrawTextLayout(D2D1::Point2F(2.0f, 1.0f), layout.Get(), bText.Get());
            } else {
                // Text (实心圆角胶囊)
                if (bBg) rt->FillRoundedRectangle(rr, bBg.Get());
                if (bBorder) rt->DrawRoundedRectangle(rr, bBorder.Get(), 1.3f);
                if (bText) rt->DrawTextLayout(D2D1::Point2F(2.0f, 1.0f), layout.Get(), bText.Get());
            }
            rt->EndDraw();
        }
    }

    cv::Mat pillBgra = cv::Mat(totalH, boxW, CV_8UC4, bits).clone();
    if (s_calloutCache.size() >= 12) {
        s_calloutCache.erase(s_calloutCache.begin());
    }
    s_calloutCache.push_back({kind, textUtf8, packedColor, innerBgr.rows, pillBgra});

    if (outBoxRadius) {
        *outBoxRadius = (std::max)(boxW, totalH) / 2;
    }
    alphaBlendPremultipliedBgraTransformedOntoBgr(pillBgra, innerBgr, x, y, scale, opacity);

    SelectObject(memDc, oldBmp);
    DeleteObject(hBmp);
    DeleteDC(memDc);
}

void drawDashedSegmentBgr(
    cv::Mat& img,
    cv::Point p0,
    cv::Point p1,
    const cv::Scalar& col,
    int thick,
    double phaseOffsetPx = 0.0,
    double revealRatio = 1.0) {
    const double totalLen = std::hypot(static_cast<double>(p1.x - p0.x), static_cast<double>(p1.y - p0.y));
    if (totalLen < 2.0) return;
    const double activeLen = totalLen * std::clamp(revealRatio, 0.0, 1.0);
    if (activeLen < 1.5) return;

    const double dashLen = (std::max)(8.0, thick * 3.2);
    const double gapLen = (std::max)(6.0, thick * 2.2);
    const double cycle = dashLen + gapLen;
    const double shift = std::fmod(std::fmod(phaseOffsetPx, cycle) + cycle, cycle);
    const double ux = (p1.x - p0.x) / totalLen;
    const double uy = (p1.y - p0.y) / totalLen;

    for (double d = shift - cycle; d < activeLen; d += cycle) {
        const double seg0 = (std::max)(0.0, d);
        const double seg1 = (std::min)(activeLen, d + dashLen);
        if (seg1 - seg0 < 1.0) continue;
        cv::Point a(static_cast<int>(std::round(p0.x + ux * seg0)), static_cast<int>(std::round(p0.y + uy * seg0)));
        cv::Point b(static_cast<int>(std::round(p0.x + ux * seg1)), static_cast<int>(std::round(p0.y + uy * seg1)));
        cv::line(img, a, b, col, thick, cv::LINE_AA);
    }
}

} // namespace

namespace compositor_detail {

void drawRoundedBoxStrokeBgr(cv::Mat& img, cv::Rect rc, int radius, const cv::Scalar& col, int thick) {
    if (rc.width < 4 || rc.height < 4) return;
    const int r = std::clamp(radius, 0, (std::min)(rc.width, rc.height) / 2);
    if (r <= 1) {
        cv::rectangle(img, rc, col, thick, cv::LINE_AA);
        return;
    }
    cv::line(img, {rc.x + r, rc.y}, {rc.x + rc.width - r, rc.y}, col, thick, cv::LINE_AA);
    cv::line(img, {rc.x + r, rc.y + rc.height}, {rc.x + rc.width - r, rc.y + rc.height}, col, thick, cv::LINE_AA);
    cv::line(img, {rc.x, rc.y + r}, {rc.x, rc.y + rc.height - r}, col, thick, cv::LINE_AA);
    cv::line(img, {rc.x + rc.width, rc.y + r}, {rc.x + rc.width, rc.y + rc.height - r}, col, thick, cv::LINE_AA);
    cv::ellipse(img, {rc.x + r, rc.y + r}, {r, r}, 180, 0, 90, col, thick, cv::LINE_AA);
    cv::ellipse(img, {rc.x + rc.width - r, rc.y + r}, {r, r}, 270, 0, 90, col, thick, cv::LINE_AA);
    cv::ellipse(img, {rc.x + rc.width - r, rc.y + rc.height - r}, {r, r}, 0, 0, 90, col, thick, cv::LINE_AA);
    cv::ellipse(img, {rc.x + r, rc.y + rc.height - r}, {r, r}, 90, 0, 90, col, thick, cv::LINE_AA);
}

void renderTimelineAnnotationsOnInner(
    cv::Mat& innerBgr,
    const cv::Mat& fullSrcBgr,
    const std::vector<StudioAnnotationItem>& annotations,
    int64_t ptsMs,
    float srcCols, float srcRows,
    float effCropX, float effCropY,
    float effCropW, float effCropH,
    bool animateAnnotations,
    int64_t annotationPreviewElapsedMs,
    int previewTargetAnnIndex) {
    if (innerBgr.empty() || annotations.empty()) return;
    const float safeCropW = (std::max)(1.0f, effCropW);
    const float safeCropH = (std::max)(1.0f, effCropH);
    const float iwF = static_cast<float>(innerBgr.cols);
    const float ihF = static_cast<float>(innerBgr.rows);
    const float strokeScale = std::clamp(ihF / 900.0f, 0.6f, 2.5f);

    auto easeOutCubic = [](double t) noexcept -> double {
        t = std::clamp(t, 0.0, 1.0);
        const double inv = 1.0 - t;
        return 1.0 - inv * inv * inv;
    };
    auto springPopScale = [](double t) noexcept -> float {
        t = std::clamp(t, 0.0, 1.0);
        if (t >= 0.999) return 1.0f;
        // 二阶阻尼微超调弹簧: 0.52 -> 1.09 -> 1.00
        return static_cast<float>(1.0 - 0.48 * std::exp(-5.4 * t) * std::cos(5.2 * t));
    };

    for (size_t annIdx = 0; annIdx < annotations.size(); ++annIdx) {
        const auto& ann = annotations[annIdx];
        const bool isLiveDemoTarget = (annotationPreviewElapsedMs >= 0 &&
                                       static_cast<int>(annIdx) == previewTargetAnnIndex);
        if (!ann.enabled) continue;
        if (!isLiveDemoTarget && (ptsMs < ann.startMs || ptsMs > ann.endMs)) continue;

        const float sx0 = ann.x0 * srcCols;
        const float sy0 = ann.y0 * srcRows;
        const float sx1 = ann.x1 * srcCols;
        const float sy1 = ann.y1 * srcRows;

        const int px0 = static_cast<int>(std::round(((sx0 - effCropX) / safeCropW) * iwF));
        const int py0 = static_cast<int>(std::round(((sy0 - effCropY) / safeCropH) * ihF));
        const int px1 = static_cast<int>(std::round(((sx1 - effCropX) / safeCropW) * iwF));
        const int py1 = static_cast<int>(std::round(((sy1 - effCropY) / safeCropH) * ihF));

        const cv::Scalar colBgr = parseHexBgrScalar(ann.colorHex, cv::Scalar(153, 72, 236));
        const int thick = (std::max)(2, static_cast<int>(std::round(ann.strokeWidth * strokeScale)));

        // ── 计算「抓眼入场 + 驻留微律动 + 优雅退场」三段式动力学包络 ──
        const double durMs = static_cast<double>((std::max<int64_t>)(240, ann.endMs - ann.startMs));
        const double localMs = isLiveDemoTarget
            ? static_cast<double>(annotationPreviewElapsedMs)
            : static_cast<double>(ptsMs - ann.startMs);

        const bool useMotion = animateAnnotations || isLiveDemoTarget;
        double enterLinear = 1.0;
        double exitLinear = 1.0;
        if (useMotion) {
            const double inSpanMs = isLiveDemoTarget ? 270.0 : std::clamp(durMs * 0.20, 120.0, 280.0);
            enterLinear = std::clamp(localMs / inSpanMs, 0.0, 1.0);
            if (!isLiveDemoTarget) {
                const double outSpanMs = std::clamp(durMs * 0.15, 90.0, 190.0);
                exitLinear = std::clamp((durMs - localMs) / outSpanMs, 0.0, 1.0);
            }
        }
        const double enterEase = easeOutCubic(enterLinear);
        const double exitEase = easeOutCubic(exitLinear);
        const double masterAlpha = std::clamp(std::min(enterLinear * 1.65, exitEase), 0.0, 1.0);
        const double animSec = useMotion ? (localMs * 0.001) : 0.45;
        const double animPhase = animSec * 7.2;

        // 隐私打码遮罩特例：从首帧至末帧 100% 严密遮挡，绝不做入退场透明或缩放以防敏感信息泄露
        if (ann.kind == StudioAnnotationKind::BlurMask) {
            const int rx0 = std::clamp((std::min)(px0, px1), 0, innerBgr.cols);
            const int ry0 = std::clamp((std::min)(py0, py1), 0, innerBgr.rows);
            const int rx1 = std::clamp((std::max)(px0, px1), 0, innerBgr.cols);
            const int ry1 = std::clamp((std::max)(py0, py1), 0, innerBgr.rows);
            const cv::Rect rc(rx0, ry0, rx1 - rx0, ry1 - ry0);
            if (rc.width >= 4 && rc.height >= 4) {
                cv::Mat roi = innerBgr(rc);
                const int cell = std::clamp(static_cast<int>(std::round(12.0f * strokeScale)), 6, 24);
                const int pixW = (std::max)(2, rc.width / cell);
                const int pixH = (std::max)(2, rc.height / cell);
                cv::Mat tiny, mosaic;
                cv::resize(roi, tiny, cv::Size(pixW, pixH), 0.0, 0.0, cv::INTER_AREA);
                cv::resize(tiny, mosaic, rc.size(), 0.0, 0.0, cv::INTER_NEAREST);
                mosaic.copyTo(roi);
            }
            continue;
        }

        if (masterAlpha <= 0.015) continue;

        // 1. 文本与步骤徽章家族 (6 款：弹簧跃出/微距上浮入场 + 分类别驻留微律动)
        if (ann.kind == StudioAnnotationKind::Text ||
            ann.kind == StudioAnnotationKind::TextPlain ||
            ann.kind == StudioAnnotationKind::TextBubble ||
            ann.kind == StudioAnnotationKind::TextDashed ||
            ann.kind == StudioAnnotationKind::TextNeon ||
            ann.kind == StudioAnnotationKind::StepBadge) {
            const int baseCx = (px0 + px1) / 2;
            const int baseCy = (py0 + py1) / 2;
            const bool isBadge = (ann.kind == StudioAnnotationKind::StepBadge);

            const float popScale = useMotion
                ? (isBadge ? springPopScale(enterLinear)
                           : static_cast<float>(0.88 + 0.12 * enterEase)) *
                  static_cast<float>(0.95 + 0.05 * exitEase)
                : 1.0f;
            int floatOffsetY = useMotion
                ? static_cast<int>(std::round((1.0 - enterEase) * 10.0 * strokeScale))
                : 0;
            if (useMotion && enterLinear >= 0.99) {
                if (ann.kind == StudioAnnotationKind::TextBubble) {
                    // 对话气泡轻柔微悬浮呼吸
                    floatOffsetY += static_cast<int>(std::round(std::sin(animSec * 3.8) * 2.8 * strokeScale));
                }
            }
            const int drawCx = baseCx;
            const int drawCy = baseCy + floatOffsetY;

            // StepBadge 专属：驻留期向外释放柔和同心雷达脉冲环，一眼锁定步骤顺序
            int approxRad = static_cast<int>(std::round(20.0f * strokeScale));
            const std::string fallback = isBadge ? "1" : "Text";
            drawStyledTextCalloutBgr(
                innerBgr, drawCx, drawCy, ann.kind,
                ann.text.empty() ? fallback : ann.text,
                colBgr, popScale, static_cast<float>(masterAlpha), &approxRad);

            if (useMotion && isBadge && enterLinear > 0.65) {
                const double pulseCycle = std::fmod( (std::max)(0.0, animSec - 0.18) * 0.90, 1.0 );
                const int ringR = approxRad + static_cast<int>(std::round(pulseCycle * 15.0 * strokeScale));
                const double ringAlpha = (1.0 - pulseCycle) * (1.0 - pulseCycle) * 0.58 * masterAlpha;
                const int pad = ringR + thick + 6;
                blendRoiOverlayBgr(
                    innerBgr, cv::Rect(drawCx - pad, drawCy - pad, pad * 2, pad * 2), ringAlpha,
                    [&](cv::Mat& sub, int ox, int oy) {
                        cv::circle(sub, {drawCx + ox, drawCy + oy}, ringR, colBgr, (std::max)(2, thick - 1), cv::LINE_AA);
                    });
            } else if (useMotion && ann.kind == StudioAnnotationKind::TextNeon && enterLinear > 0.8) {
                // 霓虹文字框外围柔和呼吸光晕
                const double glowPulse = (0.5 + 0.5 * std::sin(animSec * 5.2)) * 0.28 * masterAlpha;
                const int hw = approxRad + static_cast<int>(6 * strokeScale);
                const int hh = (std::max)(14, approxRad / 2) + static_cast<int>(4 * strokeScale);
                blendRoiOverlayBgr(
                    innerBgr, cv::Rect(drawCx - hw - 10, drawCy - hh - 10, (hw + 10) * 2, (hh + 10) * 2), glowPulse,
                    [&](cv::Mat& sub, int ox, int oy) {
                        drawRoundedBoxStrokeBgr(
                            sub, cv::Rect(drawCx - hw + ox, drawCy - hh + oy, hw * 2, hh * 2),
                            static_cast<int>(10 * strokeScale), colBgr, thick + 3);
                    });
            }
            continue;
        }

        // 2. 醒目粗体动态弹跳大箭头 (弹簧射入 + 指向呼吸弹跳 + 局部 ROI 高速阴影)
        if (ann.kind == StudioAnnotationKind::BoldArrow) {
            const double dx = static_cast<double>(px1 - px0);
            const double dy = static_cast<double>(py1 - py0);
            const double fullLen = std::hypot(dx, dy);
            const double growScale = useMotion ? std::clamp(0.30 + 0.70 * enterEase, 0.30, 1.0) : 1.0;
            const double len = fullLen * growScale;
            if (len >= 8.0) {
                const double ux = dx / fullLen;
                const double uy = dy / fullLen;
                const double nx = -uy;
                const double ny = ux;
                const double bounce = useMotion ? (std::sin(animPhase) * 6.5 * strokeScale * enterEase) : 0.0;
                const double slideIn = useMotion ? (-(1.0 - enterEase) * 18.0 * strokeScale) : 0.0;
                const double bx0 = px0 + ux * (bounce + slideIn);
                const double by0 = py0 + uy * (bounce + slideIn);
                const double bx1 = bx0 + ux * len;
                const double by1 = by0 + uy * len;

                const double headLen = std::clamp(len * 0.42, 16.0 * strokeScale, 54.0 * strokeScale);
                const double headHalfW = headLen * 0.62;
                const double shaftHalfW = headHalfW * 0.42;
                const double neckX = bx1 - ux * headLen;
                const double neckY = by1 - uy * headLen;

                std::vector<cv::Point> poly = {
                    cv::Point(static_cast<int>(std::round(bx0 + nx * shaftHalfW)), static_cast<int>(std::round(by0 + ny * shaftHalfW))),
                    cv::Point(static_cast<int>(std::round(neckX + nx * shaftHalfW)), static_cast<int>(std::round(neckY + ny * shaftHalfW))),
                    cv::Point(static_cast<int>(std::round(neckX + nx * headHalfW)), static_cast<int>(std::round(neckY + ny * headHalfW))),
                    cv::Point(static_cast<int>(std::round(bx1)), static_cast<int>(std::round(by1))),
                    cv::Point(static_cast<int>(std::round(neckX - nx * headHalfW)), static_cast<int>(std::round(neckY - ny * headHalfW))),
                    cv::Point(static_cast<int>(std::round(neckX - nx * shaftHalfW)), static_cast<int>(std::round(neckY - ny * shaftHalfW))),
                    cv::Point(static_cast<int>(std::round(bx0 - nx * shaftHalfW)), static_cast<int>(std::round(by0 - ny * shaftHalfW)))
                };
                cv::Rect bounds = cv::boundingRect(poly);
                const int shDx = static_cast<int>(std::round(3.0 * strokeScale));
                const int shDy = static_cast<int>(std::round(4.0 * strokeScale));
                bounds.x -= 8;
                bounds.y -= 8;
                bounds.width += shDx + 16;
                bounds.height += shDy + 16;

                blendRoiOverlayBgr(innerBgr, bounds, 0.32 * masterAlpha, [&](cv::Mat& sub, int ox, int oy) {
                    std::vector<cv::Point> shPoly = poly;
                    for (auto& p : shPoly) {
                        p.x += ox + shDx;
                        p.y += oy + shDy;
                    }
                    cv::fillPoly(sub, std::vector<std::vector<cv::Point>>{shPoly}, cv::Scalar(12, 14, 22), cv::LINE_AA);
                });
                blendRoiOverlayBgr(innerBgr, bounds, masterAlpha, [&](cv::Mat& sub, int ox, int oy) {
                    std::vector<cv::Point> fgPoly = poly;
                    for (auto& p : fgPoly) {
                        p.x += ox;
                        p.y += oy;
                    }
                    cv::fillPoly(sub, std::vector<std::vector<cv::Point>>{fgPoly}, colBgr, cv::LINE_AA);
                    cv::polylines(sub, std::vector<std::vector<cv::Point>>{fgPoly}, true,
                                  cv::Scalar(255, 255, 255), (std::max)(1, static_cast<int>(std::round(1.6f * strokeScale))), cv::LINE_AA);
                });
            }
            continue;
        }

        // 3. 纤细指示箭头 / 荧光划线 / 引导虚线
        if (ann.kind == StudioAnnotationKind::Arrow) {
            const double dx = static_cast<double>(px1 - px0);
            const double dy = static_cast<double>(py1 - py0);
            const double fullLen = std::hypot(dx, dy);
            if (fullLen >= 4.0) {
                const double ux = dx / fullLen;
                const double uy = dy / fullLen;
                // 入场沿轴线快速射出；驻留期做柔和指向轻叩 + 箭尖微脉冲光点
                const double drawFrac = useMotion ? std::clamp(enterEase, 0.12, 1.0) : 1.0;
                const double nudge = (useMotion && enterLinear >= 0.98)
                    ? (std::sin(animSec * 6.4) * 4.2 * strokeScale)
                    : 0.0;
                const cv::Point a0(
                    static_cast<int>(std::round(px0 + ux * nudge)),
                    static_cast<int>(std::round(py0 + uy * nudge)));
                const cv::Point a1(
                    static_cast<int>(std::round(px0 + ux * (fullLen * drawFrac + nudge))),
                    static_cast<int>(std::round(py0 + uy * (fullLen * drawFrac + nudge))));
                const double curLen = std::hypot(static_cast<double>(a1.x - a0.x), static_cast<double>(a1.y - a0.y));
                if (curLen >= 3.0) {
                    const double tipRatio = std::clamp(16.0 * strokeScale / curLen, 0.12, 0.44);
                    const int pad = static_cast<int>(26 * strokeScale) + thick;
                    const cv::Rect bounds(
                        (std::min)(a0.x, a1.x) - pad, (std::min)(a0.y, a1.y) - pad,
                        std::abs(a1.x - a0.x) + pad * 2, std::abs(a1.y - a0.y) + pad * 2);
                    blendRoiOverlayBgr(innerBgr, bounds, masterAlpha, [&](cv::Mat& sub, int ox, int oy) {
                        cv::arrowedLine(sub, {a0.x + ox, a0.y + oy}, {a1.x + ox, a1.y + oy},
                                        cv::Scalar(16, 20, 28), thick + 2, cv::LINE_AA, 0, tipRatio);
                        cv::arrowedLine(sub, {a0.x + ox, a0.y + oy}, {a1.x + ox, a1.y + oy},
                                        colBgr, thick, cv::LINE_AA, 0, tipRatio);
                    });
                    if (useMotion && enterLinear >= 0.95) {
                        const double tipWave = std::fmod((std::max)(0.0, animSec - 0.2) * 1.15, 1.0);
                        const int tipR = static_cast<int>(std::round((4.0 + tipWave * 14.0) * strokeScale));
                        const double tipAlpha = (1.0 - tipWave) * 0.52 * masterAlpha;
                        blendRoiOverlayBgr(
                            innerBgr, cv::Rect(px1 - tipR - 6, py1 - tipR - 6, (tipR + 6) * 2, (tipR + 6) * 2), tipAlpha,
                            [&](cv::Mat& sub, int ox, int oy) {
                                cv::circle(sub, {px1 + ox, py1 + oy}, tipR, colBgr, (std::max)(1, thick - 1), cv::LINE_AA);
                            });
                    }
                }
            }
            continue;
        }

        if (ann.kind == StudioAnnotationKind::GlowLine) {
            // 荧光马克笔：一笔划出 + 底层荧光呼吸 + 流光高光扫掠
            const double drawFrac = useMotion ? enterEase : 1.0;
            const cv::Point g0(px0, py0);
            const cv::Point g1(
                static_cast<int>(std::round(px0 + (px1 - px0) * drawFrac)),
                static_cast<int>(std::round(py0 + (py1 - py0) * drawFrac)));
            const int pad = thick * 4 + 12;
            const cv::Rect bounds(
                (std::min)(g0.x, g1.x) - pad, (std::min)(g0.y, g1.y) - pad,
                std::abs(g1.x - g0.x) + pad * 2, std::abs(g1.y - g0.y) + pad * 2);
            const double glowBreath = useMotion ? (0.30 + 0.14 * (0.5 + 0.5 * std::sin(animSec * 4.8))) : 0.35;
            blendRoiOverlayBgr(innerBgr, bounds, glowBreath * masterAlpha, [&](cv::Mat& sub, int ox, int oy) {
                cv::line(sub, {g0.x + ox, g0.y + oy}, {g1.x + ox, g1.y + oy}, colBgr, thick * 3 + 5, cv::LINE_AA);
            });
            blendRoiOverlayBgr(innerBgr, bounds, masterAlpha, [&](cv::Mat& sub, int ox, int oy) {
                cv::line(sub, {g0.x + ox, g0.y + oy}, {g1.x + ox, g1.y + oy}, colBgr, thick + 1, cv::LINE_AA);
            });
            if (useMotion && enterLinear >= 0.98) {
                const double sweep = std::fmod(animSec * 0.85, 1.35);
                if (sweep <= 1.0) {
                    const double s0 = (std::max)(0.0, sweep - 0.16);
                    const double s1 = (std::min)(1.0, sweep + 0.06);
                    const cv::Point sp0(
                        static_cast<int>(std::round(px0 + (px1 - px0) * s0)),
                        static_cast<int>(std::round(py0 + (py1 - py0) * s0)));
                    const cv::Point sp1(
                        static_cast<int>(std::round(px0 + (px1 - px0) * s1)),
                        static_cast<int>(std::round(py0 + (py1 - py0) * s1)));
                    blendRoiOverlayBgr(innerBgr, bounds, 0.72 * masterAlpha, [&](cv::Mat& sub, int ox, int oy) {
                        cv::line(sub, {sp0.x + ox, sp0.y + oy}, {sp1.x + ox, sp1.y + oy},
                                 cv::Scalar(255, 255, 255), (std::max)(2, thick), cv::LINE_AA);
                    });
                }
            }
            continue;
        }

        if (ann.kind == StudioAnnotationKind::DashedLine) {
            // 引导虚线：入场延伸铺开 + 驻留期定向行军流 (Marching Flow)
            const double marchPx = useMotion ? (animSec * 46.0 * strokeScale) : 0.0;
            const double reveal = useMotion ? enterEase : 1.0;
            const int pad = thick * 2 + 8;
            const cv::Rect bounds(
                (std::min)(px0, px1) - pad, (std::min)(py0, py1) - pad,
                std::abs(px1 - px0) + pad * 2, std::abs(py1 - py0) + pad * 2);
            blendRoiOverlayBgr(innerBgr, bounds, masterAlpha, [&](cv::Mat& sub, int ox, int oy) {
                drawDashedSegmentBgr(sub, {px0 + ox, py0 + oy}, {px1 + ox, py1 + oy},
                                     colBgr, thick + 1, marchPx, reveal);
            });
            continue;
        }

        // 4. 点击小手引导图标 (真实食指轻触下压节拍 + 双道扩散点击波纹环)
        if (ann.kind == StudioAnnotationKind::HandPointer) {
            const int baseHx = (px0 + px1) / 2;
            const int baseHy = (py0 + py1) / 2;
            const float s = std::clamp(strokeScale * 1.15f, 0.7f, 2.6f) *
                            (useMotion ? springPopScale(enterLinear) : 1.0f);
            const double tapCycle = useMotion ? std::fmod(animSec * 1.15, 1.0) : 0.35;
            const double tapPress = (tapCycle < 0.24)
                ? std::sin((tapCycle / 0.24) * 3.14159265)
                : 0.0;
            const int hx = baseHx;
            const int hy = baseHy + static_cast<int>(std::round(tapPress * 4.5 * s));
            const int tipY = baseHy - static_cast<int>(10 * s);

            const int pad = static_cast<int>(38 * s) + thick;
            const cv::Rect bounds(baseHx - pad, baseHy - pad, pad * 2, pad * 2);

            if (useMotion) {
                for (int wIdx = 0; wIdx < 2; ++wIdx) {
                    const double wp = std::fmod(tapCycle + wIdx * 0.22, 1.0);
                    const int pulseR = static_cast<int>(std::round((8.0 + wp * 20.0) * s));
                    const double rAlpha = (1.0 - wp) * (1.0 - wp) * 0.72 * masterAlpha;
                    blendRoiOverlayBgr(innerBgr, bounds, rAlpha, [&](cv::Mat& sub, int ox, int oy) {
                        cv::circle(sub, {baseHx + ox, tipY + oy}, pulseR, colBgr, (std::max)(2, thick), cv::LINE_AA);
                    });
                }
            } else {
                blendRoiOverlayBgr(innerBgr, bounds, 0.65, [&](cv::Mat& sub, int ox, int oy) {
                    cv::circle(sub, {baseHx + ox, tipY + oy}, static_cast<int>(16 * s), colBgr, (std::max)(2, thick), cv::LINE_AA);
                });
            }

            std::vector<cv::Point> handPts = {
                {hx - static_cast<int>(3 * s), hy - static_cast<int>(14 * s)},
                {hx + static_cast<int>(3 * s), hy - static_cast<int>(14 * s)},
                {hx + static_cast<int>(3 * s), hy - static_cast<int>(2 * s)},
                {hx + static_cast<int>(13 * s), hy},
                {hx + static_cast<int>(14 * s), hy + static_cast<int>(13 * s)},
                {hx + static_cast<int>(8 * s), hy + static_cast<int>(21 * s)},
                {hx - static_cast<int>(5 * s), hy + static_cast<int>(21 * s)},
                {hx - static_cast<int>(12 * s), hy + static_cast<int>(9 * s)},
                {hx - static_cast<int>(8 * s), hy + static_cast<int>(5 * s)},
                {hx - static_cast<int>(3 * s), hy + static_cast<int>(9 * s)}
            };
            blendRoiOverlayBgr(innerBgr, bounds, masterAlpha, [&](cv::Mat& sub, int ox, int oy) {
                for (auto& pt : handPts) {
                    pt.x += ox;
                    pt.y += oy;
                }
                cv::fillPoly(sub, std::vector<std::vector<cv::Point>>{handPts}, cv::Scalar(252, 253, 255), cv::LINE_AA);
                cv::polylines(sub, std::vector<std::vector<cv::Point>>{handPts}, true, cv::Scalar(24, 30, 42),
                              (std::max)(2, static_cast<int>(std::round(2.0f * s))), cv::LINE_AA);
            });
            continue;
        }

        const int rx0 = std::clamp((std::min)(px0, px1), 0, innerBgr.cols);
        const int ry0 = std::clamp((std::min)(py0, py1), 0, innerBgr.rows);
        const int rx1 = std::clamp((std::max)(px0, px1), 0, innerBgr.cols);
        const int ry1 = std::clamp((std::max)(py0, py1), 0, innerBgr.rows);
        const cv::Rect rc(rx0, ry0, rx1 - rx0, ry1 - ry0);

        if (ann.kind == StudioAnnotationKind::Spotlight) {
            // 剧场级聚光灯：背景暗场平滑渐合与渐退 + 聚光亮窗呼吸白边光晕
            const double dimStrength = masterAlpha * 0.64; // 峰值保留 0.36 亮度
            cv::Mat dimmed;
            innerBgr.convertTo(dimmed, -1, 1.0 - dimStrength, 0.0);
            if (rc.width > 4 && rc.height > 4) {
                cv::Mat mask(innerBgr.rows, innerBgr.cols, CV_8UC1, cv::Scalar(0));
                const int rad = static_cast<int>(std::round(10.0f * strokeScale));
                fillRoundedRectMask(mask, rc, static_cast<float>(rad));
                innerBgr.copyTo(dimmed, mask);

                if (useMotion) {
                    const double haloWave = 0.5 + 0.5 * std::sin(animSec * 4.2);
                    const int expand = static_cast<int>(std::round((2.0 + haloWave * 5.0) * strokeScale));
                    const cv::Rect haloRc(rc.x - expand, rc.y - expand, rc.width + expand * 2, rc.height + expand * 2);
                    blendRoiOverlayBgr(
                        dimmed, cv::Rect(haloRc.x - 8, haloRc.y - 8, haloRc.width + 16, haloRc.height + 16),
                        (0.18 + 0.18 * haloWave) * masterAlpha,
                        [&](cv::Mat& sub, int ox, int oy) {
                            drawRoundedBoxStrokeBgr(
                                sub, cv::Rect(haloRc.x + ox, haloRc.y + oy, haloRc.width, haloRc.height),
                                rad + expand, cv::Scalar(255, 255, 255), thick + 1);
                        });
                }
                drawRoundedBoxStrokeBgr(dimmed, rc, rad, cv::Scalar(255, 255, 255), (std::max)(1, thick - 1));
            }
            dimmed.copyTo(innerBgr);
        } else if (ann.kind == StudioAnnotationKind::Magnifier) {
            // 圆形局部光学放大镜：倍率 1.0x -> 2.2x 丝滑推近聚焦入场 + 镜框呼吸光环
            const int cx = (px0 + px1) / 2;
            const int cy = (py0 + py1) / 2;
            const double radScale = useMotion ? (0.82 + 0.18 * enterEase) : 1.0;
            const int rad = std::clamp(
                static_cast<int>(std::round(((std::max)(rc.width, rc.height) * 0.5) * radScale)),
                static_cast<int>(24 * strokeScale), (std::min)(innerBgr.cols, innerBgr.rows) / 2 - 4);
            const cv::Rect boxRc(
                std::clamp(cx - rad, 0, innerBgr.cols - 1),
                std::clamp(cy - rad, 0, innerBgr.rows - 1),
                std::clamp(cx + rad, 1, innerBgr.cols) - std::clamp(cx - rad, 0, innerBgr.cols - 1),
                std::clamp(cy + rad, 1, innerBgr.rows) - std::clamp(cy - rad, 0, innerBgr.rows - 1));
            if (boxRc.width >= 12 && boxRc.height >= 12 && !fullSrcBgr.empty()) {
                const float centerSrcX = ((ann.x0 + ann.x1) * 0.5f) * srcCols;
                const float centerSrcY = ((ann.y0 + ann.y1) * 0.5f) * srcRows;
                const float zoomMag = useMotion
                    ? static_cast<float>(1.08 + 1.12 * enterEase * exitEase)
                    : 2.2f;
                const float sampleHalfW = (static_cast<float>(rad) / iwF) * safeCropW / zoomMag;
                const float sampleHalfH = (static_cast<float>(rad) / ihF) * safeCropH / zoomMag;
                const int s0x = std::clamp(static_cast<int>(std::round(centerSrcX - sampleHalfW)), 0, fullSrcBgr.cols - 2);
                const int s0y = std::clamp(static_cast<int>(std::round(centerSrcY - sampleHalfH)), 0, fullSrcBgr.rows - 2);
                const int sw = std::clamp(static_cast<int>(std::round(sampleHalfW * 2.0f)), 2, fullSrcBgr.cols - s0x);
                const int sh = std::clamp(static_cast<int>(std::round(sampleHalfH * 2.0f)), 2, fullSrcBgr.rows - s0y);
                cv::Mat magnified;
                cv::resize(fullSrcBgr(cv::Rect(s0x, s0y, sw, sh)), magnified, cv::Size(rad * 2, rad * 2), 0.0, 0.0, cv::INTER_CUBIC);

                const int pad = rad + static_cast<int>(16 * strokeScale) + thick;
                blendRoiOverlayBgr(
                    innerBgr, cv::Rect(cx - pad, cy - pad, pad * 2, pad * 2), masterAlpha,
                    [&](cv::Mat& sub, int ox, int oy) {
                        const int lcx = cx + ox;
                        const int lcy = cy + oy;
                        cv::Mat circleMask(sub.rows, sub.cols, CV_8UC1, cv::Scalar(0));
                        cv::circle(circleMask, {lcx, lcy}, rad, cv::Scalar(255), cv::FILLED, cv::LINE_AA);

                        const int dstX0 = std::clamp(lcx - rad, 0, sub.cols - 1);
                        const int dstY0 = std::clamp(lcy - rad, 0, sub.rows - 1);
                        const int dstX1 = std::clamp(lcx + rad, 1, sub.cols);
                        const int dstY1 = std::clamp(lcy + rad, 1, sub.rows);
                        const int mx0 = dstX0 - (lcx - rad);
                        const int my0 = dstY0 - (lcy - rad);
                        const int mw = (std::min)(dstX1 - dstX0, magnified.cols - mx0);
                        const int mh = (std::min)(dstY1 - dstY0, magnified.rows - my0);
                        if (mw > 0 && mh > 0) {
                            magnified(cv::Rect(mx0, my0, mw, mh)).copyTo(
                                sub(cv::Rect(dstX0, dstY0, mw, mh)),
                                circleMask(cv::Rect(dstX0, dstY0, mw, mh)));
                        }
                        cv::circle(sub, {lcx, lcy}, rad + 1, cv::Scalar(18, 22, 32), thick + 2, cv::LINE_AA);
                        cv::circle(sub, {lcx, lcy}, rad, cv::Scalar(255, 255, 255), (std::max)(2, thick), cv::LINE_AA);
                    });

                if (useMotion && enterLinear >= 0.90) {
                    const double sonar = std::fmod((std::max)(0.0, animSec - 0.22) * 0.80, 1.0);
                    const int sRad = rad + static_cast<int>(std::round(sonar * 12.0 * strokeScale));
                    blendRoiOverlayBgr(
                        innerBgr, cv::Rect(cx - pad, cy - pad, pad * 2, pad * 2),
                        (1.0 - sonar) * 0.42 * masterAlpha,
                        [&](cv::Mat& sub, int ox, int oy) {
                            cv::circle(sub, {cx + ox, cy + oy}, sRad, cv::Scalar(255, 255, 255), (std::max)(1, thick - 1), cv::LINE_AA);
                        });
                }
            }
        } else if (ann.kind == StudioAnnotationKind::Rectangle ||
                   ann.kind == StudioAnnotationKind::RoundedBox) {
            // 直角框 / 圆角框：Direct2D 圆头抗锯齿一笔闭合勾勒 + 柔和声呐外扩光环
            if (rc.width >= 4 && rc.height >= 4) {
                const bool isRounded = (ann.kind == StudioAnnotationKind::RoundedBox);
                const float radF = isRounded
                    ? std::clamp(14.0f * strokeScale, 4.0f, static_cast<float>((std::min)(rc.width, rc.height)) * 0.34f)
                    : 2.0f;
                const float drawProg = static_cast<float>(useMotion ? enterEase : 1.0);
                const float fThick = (std::max)(2.2f, ann.strokeWidth * strokeScale);
                const int pad = static_cast<int>(22 * strokeScale) + thick * 2;
                const cv::Rect bounds(rc.x - pad, rc.y - pad, rc.width + pad * 2, rc.height + pad * 2);
                const float cr = static_cast<float>(colBgr[2] / 255.0);
                const float cg = static_cast<float>(colBgr[1] / 255.0);
                const float cb = static_cast<float>(colBgr[0] / 255.0);

                renderD2DRoiVectorOntoBgr(innerBgr, bounds, static_cast<float>(masterAlpha),
                    [&](ID2D1Factory* fac, ID2D1RenderTarget* rt, ID2D1SolidColorBrush* br,
                        ID2D1StrokeStyle* roundSt, float ox, float oy) {
                        const D2D1_RECT_F box = D2D1::RectF(
                            rc.x + ox, rc.y + oy, rc.x + rc.width + ox, rc.y + rc.height + oy);

                        // 驻留期柔和外扩呼吸光晕
                        if (useMotion && enterLinear >= 0.92) {
                            const float sonar = static_cast<float>(std::fmod((std::max)(0.0, animSec - 0.24) * 0.78, 1.0));
                            const float exp = sonar * 11.0f * strokeScale;
                            const D2D1_RECT_F pBox = D2D1::RectF(box.left - exp, box.top - exp, box.right + exp, box.bottom + exp);
                            br->SetColor(D2D1::ColorF(cr, cg, cb, (1.0f - sonar) * (1.0f - sonar) * 0.48f));
                            rt->DrawRoundedRectangle(D2D1::RoundedRect(pBox, radF + exp, radF + exp), br, (std::max)(1.4f, fThick * 0.65f));
                        }

                        if (drawProg >= 0.995f) {
                            // 完整闭合超平滑圆角/微圆角框 + 立体柔和衬底描边
                            br->SetColor(D2D1::ColorF(0.05f, 0.07f, 0.12f, 0.34f));
                            rt->DrawRoundedRectangle(
                                D2D1::RoundedRect(D2D1::RectF(box.left + 1.0f, box.top + 1.8f, box.right + 1.0f, box.bottom + 1.8f), radF, radF),
                                br, fThick + 1.6f);
                            br->SetColor(D2D1::ColorF(cr, cg, cb, 0.98f));
                            rt->DrawRoundedRectangle(D2D1::RoundedRect(box, radF, radF), br, fThick);
                        } else if (fac) {
                            // 入场顺时针连续圆头几何路径一笔勾勒
                            constexpr int kSteps = 80;
                            const int activePts = (std::max)(2, static_cast<int>(std::round(kSteps * drawProg)));
                            const float cx = (box.left + box.right) * 0.5f;
                            const float cy = (box.top + box.bottom) * 0.5f;
                            const float hw = (box.right - box.left) * 0.5f;
                            const float hh = (box.bottom - box.top) * 0.5f;
                            const float r = (std::min)({radF, hw * 0.95f, hh * 0.95f});

                            // 构造顺时针超椭圆/圆角矩形连续参数采样点
                            auto sampleBoxPerim = [&](float u) -> D2D1_POINT_2F {
                                const float topW = (std::max)(0.0f, (hw - r) * 2.0f);
                                const float sideH = (std::max)(0.0f, (hh - r) * 2.0f);
                                const float arcL = 1.5707963f * r;
                                const float totalP = (topW + sideH) * 2.0f + arcL * 4.0f;
                                float d = std::clamp(u, 0.0f, 1.0f) * totalP;
                                if (d <= topW) return {box.left + r + d, box.top};
                                d -= topW;
                                if (d <= arcL) {
                                    const float a = -1.5707963f + (d / (std::max)(0.01f, arcL)) * 1.5707963f;
                                    return {box.right - r + std::cos(a) * r, box.top + r + std::sin(a) * r};
                                }
                                d -= arcL;
                                if (d <= sideH) return {box.right, box.top + r + d};
                                d -= sideH;
                                if (d <= arcL) {
                                    const float a = (d / (std::max)(0.01f, arcL)) * 1.5707963f;
                                    return {box.right - r + std::cos(a) * r, box.bottom - r + std::sin(a) * r};
                                }
                                d -= arcL;
                                if (d <= topW) return {box.right - r - d, box.bottom};
                                d -= topW;
                                if (d <= arcL) {
                                    const float a = 1.5707963f + (d / (std::max)(0.01f, arcL)) * 1.5707963f;
                                    return {box.left + r + std::cos(a) * r, box.bottom - r + std::sin(a) * r};
                                }
                                d -= arcL;
                                if (d <= sideH) return {box.left, box.bottom - r - d};
                                d -= sideH;
                                const float a = 3.1415926f + (d / (std::max)(0.01f, arcL)) * 1.5707963f;
                                return {box.left + r + std::cos(a) * r, box.top + r + std::sin(a) * r};
                            };

                            ComPtr<ID2D1PathGeometry> geo;
                            if (SUCCEEDED(fac->CreatePathGeometry(geo.GetAddressOf())) && geo) {
                                ComPtr<ID2D1GeometrySink> sink;
                                if (SUCCEEDED(geo->Open(sink.GetAddressOf())) && sink) {
                                    sink->BeginFigure(sampleBoxPerim(0.0f), D2D1_FIGURE_BEGIN_HOLLOW);
                                    for (int i = 1; i <= activePts; ++i) {
                                        sink->AddLine(sampleBoxPerim(static_cast<float>(i) / static_cast<float>(kSteps)));
                                    }
                                    sink->EndFigure(D2D1_FIGURE_END_OPEN);
                                    sink->Close();
                                    br->SetColor(D2D1::ColorF(0.05f, 0.07f, 0.12f, 0.32f));
                                    rt->DrawGeometry(geo.Get(), br, fThick + 1.6f, roundSt);
                                    br->SetColor(D2D1::ColorF(cr, cg, cb, 0.98f));
                                    rt->DrawGeometry(geo.Get(), br, fThick, roundSt);
                                }
                            }
                            (void)cx;
                            (void)cy;
                        }
                    });
            }
        } else if (ann.kind == StudioAnnotationKind::CircleRing) {
            // 标准圆环/椭圆聚焦环：Direct2D 超平滑圆头弧线顺时针一笔闭合 + 同心声呐聚焦波
            if (rc.width >= 4 && rc.height >= 4) {
                const float cx = rc.x + rc.width * 0.5f;
                const float cy = rc.y + rc.height * 0.5f;
                const float rx = rc.width * 0.5f;
                const float ry = rc.height * 0.5f;
                const float drawProg = static_cast<float>(useMotion ? enterEase : 1.0);
                const float fThick = (std::max)(2.2f, ann.strokeWidth * strokeScale);
                const int pad = static_cast<int>(22 * strokeScale) + thick * 2;
                const cv::Rect bounds(rc.x - pad, rc.y - pad, rc.width + pad * 2, rc.height + pad * 2);
                const float cr = static_cast<float>(colBgr[2] / 255.0);
                const float cg = static_cast<float>(colBgr[1] / 255.0);
                const float cb = static_cast<float>(colBgr[0] / 255.0);

                renderD2DRoiVectorOntoBgr(innerBgr, bounds, static_cast<float>(masterAlpha),
                    [&](ID2D1Factory* fac, ID2D1RenderTarget* rt, ID2D1SolidColorBrush* br,
                        ID2D1StrokeStyle* roundSt, float ox, float oy) {
                        const D2D1_POINT_2F cPt{cx + ox, cy + oy};
                        if (useMotion && enterLinear >= 0.92) {
                            const float sonar = static_cast<float>(std::fmod((std::max)(0.0, animSec - 0.24) * 0.80, 1.0));
                            const float exp = sonar * 11.0f * strokeScale;
                            br->SetColor(D2D1::ColorF(cr, cg, cb, (1.0f - sonar) * (1.0f - sonar) * 0.48f));
                            rt->DrawEllipse(D2D1::Ellipse(cPt, rx + exp, ry + exp), br, (std::max)(1.4f, fThick * 0.65f));
                        }
                        if (drawProg >= 0.995f) {
                            br->SetColor(D2D1::ColorF(0.05f, 0.07f, 0.12f, 0.34f));
                            rt->DrawEllipse(D2D1::Ellipse({cPt.x + 1.0f, cPt.y + 1.8f}, rx, ry), br, fThick + 1.6f);
                            br->SetColor(D2D1::ColorF(cr, cg, cb, 0.98f));
                            rt->DrawEllipse(D2D1::Ellipse(cPt, rx, ry), br, fThick);
                        } else if (fac) {
                            constexpr int kSteps = 96;
                            const int activePts = (std::max)(2, static_cast<int>(std::round(kSteps * drawProg)));
                            ComPtr<ID2D1PathGeometry> geo;
                            if (SUCCEEDED(fac->CreatePathGeometry(geo.GetAddressOf())) && geo) {
                                ComPtr<ID2D1GeometrySink> sink;
                                if (SUCCEEDED(geo->Open(sink.GetAddressOf())) && sink) {
                                    auto ptAt = [&](int idx) -> D2D1_POINT_2F {
                                        const float ang = -1.5707963f + (static_cast<float>(idx) / kSteps) * 6.2831853f;
                                        return {cPt.x + std::cos(ang) * rx, cPt.y + std::sin(ang) * ry};
                                    };
                                    sink->BeginFigure(ptAt(0), D2D1_FIGURE_BEGIN_HOLLOW);
                                    for (int i = 1; i <= activePts; ++i) sink->AddLine(ptAt(i));
                                    sink->EndFigure(D2D1_FIGURE_END_OPEN);
                                    sink->Close();
                                    br->SetColor(D2D1::ColorF(0.05f, 0.07f, 0.12f, 0.32f));
                                    rt->DrawGeometry(geo.Get(), br, fThick + 1.6f, roundSt);
                                    br->SetColor(D2D1::ColorF(cr, cg, cb, 0.98f));
                                    rt->DrawGeometry(geo.Get(), br, fThick, roundSt);
                                }
                            }
                        }
                    });
            }
        } else if (ann.kind == StudioAnnotationKind::SketchCircle) {
            // 手绘重点圈 (Hand-Drawn Marker Loop)：
            // 彻底摒弃旧版双错位 OpenCV ellipse (>360° 截断撕裂破碎)，升级为 Direct2D 原生圆头抗锯齿单笔流畅马克笔圈重点曲线！
            if (rc.width >= 8 && rc.height >= 8) {
                const float cx = rc.x + rc.width * 0.5f;
                const float cy = rc.y + rc.height * 0.5f;
                const float rx = rc.width * 0.5f;
                const float ry = rc.height * 0.5f;
                const float drawProg = static_cast<float>(useMotion ? easeOutCubic(std::clamp(enterLinear / 0.88, 0.0, 1.0)) : 1.0);
                const float fThick = (std::max)(2.8f, ann.strokeWidth * 1.12f * strokeScale);
                const int pad = static_cast<int>(26 * strokeScale) + thick * 3;
                const cv::Rect bounds(rc.x - pad, rc.y - pad, rc.width + pad * 2, rc.height + pad * 2);
                const float cr = static_cast<float>(colBgr[2] / 255.0);
                const float cg = static_cast<float>(colBgr[1] / 255.0);
                const float cb = static_cast<float>(colBgr[0] / 255.0);

                if (drawProg > 0.015f) {
                    renderD2DRoiVectorOntoBgr(innerBgr, bounds, static_cast<float>(masterAlpha),
                        [&](ID2D1Factory* fac, ID2D1RenderTarget* rt, ID2D1SolidColorBrush* br,
                            ID2D1StrokeStyle* roundSt, float ox, float oy) {
                            if (!fac) return;
                            const float lcx = cx + ox;
                            const float lcy = cy + oy;
                            // 顺时针一气呵成画 392°（一整圈 + 顶部 32° 自然微距外飘优雅叠尾，零交叉打结、零缺口断裂）
                            constexpr int kTotalSamples = 128;
                            const int endSample = (std::max)(3, static_cast<int>(std::round(kTotalSamples * drawProg)));
                            const float tiltRad = -0.052f; // -3.0° 自然手写微倾角
                            const float cosTilt = std::cos(tiltRad);
                            const float sinTilt = std::sin(tiltRad);

                            auto evalMarkerPt = [&](float u, float dxOff = 0.0f, float dyOff = 0.0f) -> D2D1_POINT_2F {
                                // u in [0, 1] -> theta from -108 deg (-1.885 rad) clockwise by 392 deg (6.842 rad)
                                const float theta = -1.885f + u * 6.842f;
                                // 半径从 0.965 平滑渐扩至 1.055，使首尾交叠 32° 保持柔和平行微距，浑然天成
                                const float spiral = 0.965f + 0.090f * u;
                                const float organic = 1.0f + 0.012f * std::sin(theta * 2.0f + 0.6f);
                                const float ex = std::cos(theta) * rx * spiral * organic;
                                const float ey = std::sin(theta) * ry * spiral * organic;
                                return {
                                    lcx + ex * cosTilt - ey * sinTilt + dxOff,
                                    lcy + ex * sinTilt + ey * cosTilt + dyOff
                                };
                            };

                            auto buildSubPath = [&](int s0, int s1, float dxOff, float dyOff) -> ComPtr<ID2D1PathGeometry> {
                                ComPtr<ID2D1PathGeometry> g;
                                if (s1 <= s0 || FAILED(fac->CreatePathGeometry(g.GetAddressOf())) || !g) return nullptr;
                                ComPtr<ID2D1GeometrySink> sk;
                                if (SUCCEEDED(g->Open(sk.GetAddressOf())) && sk) {
                                    sk->BeginFigure(evalMarkerPt(static_cast<float>(s0) / kTotalSamples, dxOff, dyOff),
                                                    D2D1_FIGURE_BEGIN_HOLLOW);
                                    for (int i = s0 + 1; i <= s1; ++i) {
                                        sk->AddLine(evalMarkerPt(static_cast<float>(i) / kTotalSamples, dxOff, dyOff));
                                    }
                                    sk->EndFigure(D2D1_FIGURE_END_OPEN);
                                    sk->Close();
                                }
                                return g;
                            };

                            const ComPtr<ID2D1PathGeometry> shadowGeo =
                                buildSubPath(0, endSample, 1.4f * strokeScale, 2.2f * strokeScale);
                            const ComPtr<ID2D1PathGeometry> mainGeo =
                                buildSubPath(0, endSample, 0.0f, 0.0f);
                            if (!mainGeo) return;

                            // 1) 柔和立体暗影托底（深浅背景皆清晰醒目）
                            if (shadowGeo) {
                                br->SetColor(D2D1::ColorF(0.04f, 0.06f, 0.11f, 0.22f));
                                rt->DrawGeometry(shadowGeo.Get(), br, fThick + 1.2f, roundSt);
                            }

                            // 2) 外层轻柔马克笔边缘光晕（驻留期极轻柔呼吸）
                            const float glowBreath = useMotion
                                ? static_cast<float>(0.09 + 0.08 * (0.5 + 0.5 * std::sin(animSec * 3.8)))
                                : 0.10f;
                            br->SetColor(D2D1::ColorF(cr, cg, cb, glowBreath));
                            rt->DrawGeometry(mainGeo.Get(), br, fThick * 2.1f, roundSt);

                            // 3) 主体纯正饱满实心圆头马克笔迹（零夹心白线，白底与深底皆纯净醒目）
                            br->SetColor(D2D1::ColorF(cr, cg, cb, 0.98f));
                            rt->DrawGeometry(mainGeo.Get(), br, fThick, roundSt);
                        });
                }
            }
        }
    }
}

// 统一解码源视频并按真实 PTS 时间戳重采样输出每一帧合成后的 BGR 画布

} // namespace compositor_detail

} // namespace tools3000::capture
