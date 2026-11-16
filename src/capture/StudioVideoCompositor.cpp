#include "capture/StudioVideoCompositor.h"
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

#include <opencv2/imgproc.hpp>

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

namespace tools3000::capture {

namespace {

using Microsoft::WRL::ComPtr;

struct GradientStopPalette {
    cv::Vec3f c0; // BGR 0..255
    cv::Vec3f c1;
    cv::Vec3f c2;
};

GradientStopPalette paletteForTheme(BeautyBackgroundType theme) noexcept {
    switch (theme) {
        case BeautyBackgroundType::PaperChalk: // Sunrise Coral: #FB7185 -> #F97316 -> #FACC15
            return {{133.0f, 113.0f, 251.0f}, {22.0f, 115.0f, 249.0f}, {21.0f, 204.0f, 250.0f}};
        case BeautyBackgroundType::SilkMist: // Emerald Sea: #0EA5E9 -> #14B8A6 -> #10B981
            return {{233.0f, 165.0f, 14.0f}, {166.0f, 184.0f, 20.0f}, {129.0f, 185.0f, 16.0f}};
        case BeautyBackgroundType::MidnightGraphite: // Obsidian Dark: #334155 -> #1E293B -> #090D16
            return {{85.0f, 65.0f, 51.0f}, {59.0f, 41.0f, 30.0f}, {22.0f, 13.0f, 9.0f}};
        case BeautyBackgroundType::PureMinimal: // Minimal Light: #F8FAFC -> #E2E8F0 -> #CBD5E1
            return {{252.0f, 250.0f, 248.0f}, {240.0f, 232.0f, 226.0f}, {225.0f, 213.0f, 203.0f}};
        case BeautyBackgroundType::StudioSlate:
        default: // Aurora Purple: #6366F1 -> #8B5CF6 -> #EC4899
            return {{241.0f, 102.0f, 99.0f}, {246.0f, 92.0f, 139.0f}, {153.0f, 72.0f, 236.0f}};
    }
}

void fillRoundedRectMask(cv::Mat& maskU8, cv::Rect rect, float radius) {
    if (rect.width <= 0 || rect.height <= 0) return;
    const int r = static_cast<int>(std::round(
        std::clamp(radius, 0.0f, static_cast<float>((std::min)(rect.width, rect.height)) * 0.5f)));
    if (r <= 1) {
        cv::rectangle(maskU8, rect, cv::Scalar(255), cv::FILLED);
        return;
    }
    cv::rectangle(maskU8, cv::Rect(rect.x + r, rect.y, rect.width - 2 * r, rect.height), cv::Scalar(255), cv::FILLED);
    cv::rectangle(maskU8, cv::Rect(rect.x, rect.y + r, rect.width, rect.height - 2 * r), cv::Scalar(255), cv::FILLED);
    cv::circle(maskU8, cv::Point(rect.x + r, rect.y + r), r, cv::Scalar(255), cv::FILLED, cv::LINE_AA);
    cv::circle(maskU8, cv::Point(rect.x + rect.width - r - 1, rect.y + r), r, cv::Scalar(255), cv::FILLED, cv::LINE_AA);
    cv::circle(maskU8, cv::Point(rect.x + r, rect.y + rect.height - r - 1), r, cv::Scalar(255), cv::FILLED, cv::LINE_AA);
    cv::circle(maskU8, cv::Point(rect.x + rect.width - r - 1, rect.y + rect.height - r - 1), r, cv::Scalar(255), cv::FILLED, cv::LINE_AA);
}

void addCodecIfPresent(std::vector<const AVCodec*>& list, const char* name) {
    const AVCodec* c = avcodec_find_encoder_by_name(name);
    if (!c) return;
    for (const auto* existing : list) {
        if (existing == c || std::strcmp(existing->name, c->name) == 0) return;
    }
    list.push_back(c);
}

AVPixelFormat selectStudioEncoderPixFmt(const AVCodec* codec) {
    const bool isMf = codec && std::strstr(codec->name, "_mf") != nullptr;
    const void* configs = nullptr;
    int count = 0;
    if (avcodec_get_supported_config(nullptr, codec, AV_CODEC_CONFIG_PIX_FORMAT, 0, &configs, &count) < 0 ||
        !configs || count <= 0) {
        return isMf ? AV_PIX_FMT_NV12 : AV_PIX_FMT_YUV420P;
    }
    const auto* fmts = static_cast<const AVPixelFormat*>(configs);
    const std::array<AVPixelFormat, 3> pref = isMf
        ? std::array<AVPixelFormat, 3>{AV_PIX_FMT_NV12, AV_PIX_FMT_YUV420P, AV_PIX_FMT_YUVJ420P}
        : std::array<AVPixelFormat, 3>{AV_PIX_FMT_YUV420P, AV_PIX_FMT_NV12, AV_PIX_FMT_YUVJ420P};
    for (auto target : pref) {
        for (int i = 0; i < count; ++i) {
            if (fmts[i] == target) return target;
        }
    }
    return fmts[0];
}

void alphaBlendPremultipliedBgraOntoBgr(const cv::Mat& srcBgra, cv::Mat& dstBgr, int dstX, int dstY) {
    if (srcBgra.empty() || srcBgra.type() != CV_8UC4 || dstBgr.empty() || dstBgr.type() != CV_8UC3) return;
    const int x0 = (std::max)(0, dstX);
    const int y0 = (std::max)(0, dstY);
    const int x1 = (std::min)(dstBgr.cols, dstX + srcBgra.cols);
    const int y1 = (std::min)(dstBgr.rows, dstY + srcBgra.rows);
    if (x1 <= x0 || y1 <= y0) return;

    for (int y = y0; y < y1; ++y) {
        const uint8_t* sRow = srcBgra.ptr<uint8_t>(y - dstY) + (x0 - dstX) * 4;
        uint8_t* dRow = dstBgr.ptr<uint8_t>(y) + x0 * 3;
        for (int x = x0; x < x1; ++x) {
            const uint32_t a = sRow[3];
            if (a == 255) {
                dRow[0] = sRow[0];
                dRow[1] = sRow[1];
                dRow[2] = sRow[2];
            } else if (a > 0) {
                const uint32_t invA = 255 - a;
                // srcBgra 来自 D2D 预乘 Alpha: out = src_premul + dst * (1 - a)
                dRow[0] = static_cast<uint8_t>((std::min<uint32_t>)(255, sRow[0] + (dRow[0] * invA + 127) / 255));
                dRow[1] = static_cast<uint8_t>((std::min<uint32_t>)(255, sRow[1] + (dRow[1] * invA + 127) / 255));
                dRow[2] = static_cast<uint8_t>((std::min<uint32_t>)(255, sRow[2] + (dRow[2] * invA + 127) / 255));
            }
            sRow += 4;
            dRow += 3;
        }
    }
}

} // namespace

cv::Size StudioVideoCompositor::resolveOutputCanvasSize(int srcWidth, int srcHeight) noexcept {
    int w = (std::max)(64, srcWidth);
    int h = (std::max)(64, srcHeight);
    if (w & 1) ++w;
    if (h & 1) ++h;
    return {w, h};
}

void StudioVideoCompositor::ensureBackdropCache(
    int outW, int outH,
    bool autoFrame,
    BeautyBackgroundType theme,
    int padX, int padY,
    int innerW, int innerH,
    float radius) {
    if (!m_cachedBackdropBgr.empty() &&
        m_cacheW == outW && m_cacheH == outH &&
        m_cacheAutoFrame == autoFrame &&
        m_cacheTheme == theme &&
        m_cachePadX == padX && m_cachePadY == padY &&
        m_cacheInnerW == innerW && m_cacheInnerH == innerH &&
        std::abs(m_cacheRadius - radius) < 0.2f) {
        return;
    }

    m_cacheW = outW;
    m_cacheH = outH;
    m_cacheAutoFrame = autoFrame;
    m_cacheTheme = theme;
    m_cachePadX = padX;
    m_cachePadY = padY;
    m_cacheInnerW = innerW;
    m_cacheInnerH = innerH;
    m_cacheRadius = radius;

    m_cachedBackdropBgr.create(outH, outW, CV_8UC3);
    if (!autoFrame) {
        m_cachedBackdropBgr.setTo(cv::Scalar(16, 18, 24));
        return;
    }

    const GradientStopPalette pal = paletteForTheme(theme);
    const float invDiag = 1.0f / (std::max)(1.0f, static_cast<float>(outW + outH));
    for (int y = 0; y < outH; ++y) {
        uint8_t* row = m_cachedBackdropBgr.ptr<uint8_t>(y);
        for (int x = 0; x < outW; ++x) {
            const float t = std::clamp(static_cast<float>(x + y) * invDiag, 0.0f, 1.0f);
            cv::Vec3f col;
            if (t < 0.5f) {
                const float k = t * 2.0f;
                col = pal.c0 * (1.0f - k) + pal.c1 * k;
            } else {
                const float k = (t - 0.5f) * 2.0f;
                col = pal.c1 * (1.0f - k) + pal.c2 * k;
            }
            row[x * 3 + 0] = static_cast<uint8_t>(std::clamp(col[0], 0.0f, 255.0f));
            row[x * 3 + 1] = static_cast<uint8_t>(std::clamp(col[1], 0.0f, 255.0f));
            row[x * 3 + 2] = static_cast<uint8_t>(std::clamp(col[2], 0.0f, 255.0f));
        }
    }

    // 生成柔和立体投影
    cv::Mat shadowMask(outH, outW, CV_8UC1, cv::Scalar(0));
    const int offsetY = (std::max)(4, outH / 75);
    const cv::Rect shadowRect(
        padX,
        (std::min)(outH - innerH, padY + offsetY),
        innerW,
        innerH);
    fillRoundedRectMask(shadowMask, shadowRect, radius);
    int blurKernel = (std::max)(15, (outH / 24) | 1);
    if ((blurKernel & 1) == 0) ++blurKernel;
    cv::GaussianBlur(shadowMask, shadowMask, cv::Size(blurKernel, blurKernel), 0.0);

    for (int y = 0; y < outH; ++y) {
        uint8_t* bgrRow = m_cachedBackdropBgr.ptr<uint8_t>(y);
        const uint8_t* shRow = shadowMask.ptr<uint8_t>(y);
        for (int x = 0; x < outW; ++x) {
            const float shAlpha = (static_cast<float>(shRow[x]) / 255.0f) * 0.44f;
            if (shAlpha > 0.002f) {
                const float keep = 1.0f - shAlpha;
                bgrRow[x * 3 + 0] = static_cast<uint8_t>(bgrRow[x * 3 + 0] * keep);
                bgrRow[x * 3 + 1] = static_cast<uint8_t>(bgrRow[x * 3 + 1] * keep);
                bgrRow[x * 3 + 2] = static_cast<uint8_t>(bgrRow[x * 3 + 2] * keep);
            }
        }
    }
}

void StudioVideoCompositor::ensureCaptionOverlayCache(const std::string& captionUtf8, int canvasW, int canvasH) {
    if (captionUtf8.empty()) {
        m_cachedCaptionText.clear();
        m_cachedCaptionBgra.release();
        return;
    }
    if (captionUtf8 == m_cachedCaptionText && canvasW == m_cachedCaptionCanvasW && !m_cachedCaptionBgra.empty()) {
        return;
    }

    m_cachedCaptionText = captionUtf8;
    m_cachedCaptionCanvasW = canvasW;
    m_cachedCaptionBgra.release();

    const std::wstring wide = tools3000::core::WinUtils::utf8ToWstring(captionUtf8);
    if (wide.empty()) return;

    ComPtr<ID2D1Factory> d2dFactory;
    ComPtr<IDWriteFactory> dwFactory;
    if (FAILED(D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, d2dFactory.GetAddressOf())) ||
        FAILED(DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory),
                                   reinterpret_cast<IUnknown**>(dwFactory.GetAddressOf())))) {
        return;
    }

    const float fontSize = std::clamp(static_cast<float>(canvasH) * 0.029f, 14.0f, 34.0f);
    const wchar_t* fontFamily = tools3000::core::resolveInstalledUiFontFamily(dwFactory.Get());
    ComPtr<IDWriteTextFormat> fmt;
    if (FAILED(dwFactory->CreateTextFormat(
            fontFamily, nullptr, DWRITE_FONT_WEIGHT_SEMI_BOLD,
            DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, fontSize, L"zh-CN", fmt.GetAddressOf())) || !fmt) {
        return;
    }
    fmt->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
    fmt->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);

    const float maxTextW = static_cast<float>(canvasW) * 0.78f;
    const float maxTextH = static_cast<float>(canvasH) * 0.22f;
    ComPtr<IDWriteTextLayout> layout;
    if (FAILED(dwFactory->CreateTextLayout(
            wide.c_str(), static_cast<UINT32>(wide.size()), fmt.Get(), maxTextW, maxTextH, layout.GetAddressOf()))) {
        return;
    }

    DWRITE_TEXT_METRICS metrics{};
    if (FAILED(layout->GetMetrics(&metrics))) return;

    const float padX = std::clamp(fontSize * 0.95f, 14.0f, 28.0f);
    const float padY = std::clamp(fontSize * 0.45f, 7.0f, 16.0f);
    const int pillW = (std::min)(canvasW - 16, static_cast<int>(std::ceil(metrics.widthIncludingTrailingWhitespace + padX * 2.0f + 8.0f)));
    const int pillH = (std::min)(canvasH / 3, static_cast<int>(std::ceil(metrics.height + padY * 2.0f + 6.0f)));
    if (pillW <= 8 || pillH <= 8) return;

    // 调整 Layout 盒宽为精确胶囊宽以居中渲染
    layout.Reset();
    if (FAILED(dwFactory->CreateTextLayout(
            wide.c_str(), static_cast<UINT32>(wide.size()), fmt.Get(),
            static_cast<float>(pillW - 4), static_cast<float>(pillH - 4), layout.GetAddressOf()))) {
        return;
    }

    BITMAPINFO bmi{};
    bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bmi.bmiHeader.biWidth = pillW;
    bmi.bmiHeader.biHeight = -pillH; // Top-down
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
    std::memset(bits, 0, static_cast<size_t>(pillW) * static_cast<size_t>(pillH) * 4);

    D2D1_RENDER_TARGET_PROPERTIES props = D2D1::RenderTargetProperties(
        D2D1_RENDER_TARGET_TYPE_DEFAULT,
        D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED));
    ComPtr<ID2D1DCRenderTarget> rt;
    if (SUCCEEDED(d2dFactory->CreateDCRenderTarget(&props, rt.GetAddressOf())) && rt) {
        RECT bindRc{0, 0, pillW, pillH};
        if (SUCCEEDED(rt->BindDC(memDc, &bindRc))) {
            rt->BeginDraw();
            rt->SetTextAntialiasMode(D2D1_TEXT_ANTIALIAS_MODE_GRAYSCALE);
            rt->Clear(D2D1::ColorF(0, 0, 0, 0));

            const float radius = (std::min)(static_cast<float>(pillH) * 0.46f, 16.0f);
            D2D1_ROUNDED_RECT rr = D2D1::RoundedRect(
                D2D1::RectF(1.0f, 1.0f, static_cast<float>(pillW) - 1.0f, static_cast<float>(pillH) - 1.0f),
                radius, radius);

            ComPtr<ID2D1SolidColorBrush> bgBrush;
            ComPtr<ID2D1SolidColorBrush> borderBrush;
            ComPtr<ID2D1SolidColorBrush> textBrush;
            rt->CreateSolidColorBrush(D2D1::ColorF(0.06f, 0.08f, 0.12f, 0.84f), bgBrush.GetAddressOf());
            rt->CreateSolidColorBrush(D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.22f), borderBrush.GetAddressOf());
            rt->CreateSolidColorBrush(D2D1::ColorF(0.98f, 0.99f, 1.0f, 1.0f), textBrush.GetAddressOf());

            if (bgBrush) rt->FillRoundedRectangle(rr, bgBrush.Get());
            if (borderBrush) rt->DrawRoundedRectangle(rr, borderBrush.Get(), 1.0f);
            if (textBrush) rt->DrawTextLayout(D2D1::Point2F(2.0f, 2.0f), layout.Get(), textBrush.Get());
            rt->EndDraw();
        }
    }

    m_cachedCaptionBgra = cv::Mat(pillH, pillW, CV_8UC4, bits).clone();
    m_cachedCaptionDrawX = (canvasW - pillW) / 2;
    m_cachedCaptionDrawY = canvasH - pillH - (std::max)(18, static_cast<int>(canvasH * 0.055f));

    SelectObject(memDc, oldBmp);
    DeleteObject(hBmp);
    DeleteDC(memDc);
}

void StudioVideoCompositor::drawCrispVectorCursor(
    cv::Mat& canvasBgr,
    float x, float y,
    StudioCursorKind kind,
    float scale,
    float rippleProgress,
    bool rippleRight) {
    if (kind == StudioCursorKind::Hidden || canvasBgr.empty()) return;
    if (x < -32.0f || y < -32.0f || x > canvasBgr.cols + 32.0f || y > canvasBgr.rows + 32.0f) return;

    constexpr int kShift = 4;
    constexpr float kSub = 16.0f;
    const auto toSubPt = [&](float dx, float dy) -> cv::Point {
        return {
            static_cast<int>(std::round((x + dx * scale) * kSub)),
            static_cast<int>(std::round((y + dy * scale) * kSub))
        };
    };

    // 1. 点击扩散柔光波纹
    if (rippleProgress >= 0.0f && rippleProgress <= 1.0f) {
        const float ease = 1.0f - (1.0f - rippleProgress) * (1.0f - rippleProgress);
        const float radius = (10.0f + 34.0f * ease) * std::clamp(scale, 0.85f, 1.8f);
        const float alpha = (1.0f - rippleProgress) * 0.62f;
        const int rInt = static_cast<int>(std::ceil(radius + 4.0f));
        const int rx0 = (std::max)(0, static_cast<int>(x) - rInt);
        const int ry0 = (std::max)(0, static_cast<int>(y) - rInt);
        const int rx1 = (std::min)(canvasBgr.cols, static_cast<int>(x) + rInt);
        const int ry1 = (std::min)(canvasBgr.rows, static_cast<int>(y) + rInt);
        if (rx1 > rx0 && ry1 > ry0 && alpha > 0.02f) {
            cv::Mat roi = canvasBgr(cv::Rect(rx0, ry0, rx1 - rx0, ry1 - ry0));
            cv::Mat overlay = roi.clone();
            const cv::Scalar ringColor = rippleRight
                ? cv::Scalar(11, 158, 245)   // Amber #F59E0B in BGR
                : cv::Scalar(248, 189, 56);  // Sky #38BDF8 in BGR
            cv::circle(
                overlay,
                cv::Point(static_cast<int>((x - rx0) * kSub), static_cast<int>((y - ry0) * kSub)),
                static_cast<int>(radius * kSub),
                ringColor,
                (std::max)(2, static_cast<int>(std::round(2.6f * scale))),
                cv::LINE_AA,
                kShift);
            cv::addWeighted(overlay, alpha, roi, 1.0 - alpha, 0.0, roi);
        }
    }

    if (kind == StudioCursorKind::IBeam) {
        const int thickBg = (std::max)(3, static_cast<int>(std::round(3.6f * scale)));
        const int thickFg = (std::max)(1, static_cast<int>(std::round(1.6f * scale)));
        const cv::Point topL = toSubPt(-4.5f, -9.5f);
        const cv::Point topR = toSubPt(4.5f, -9.5f);
        const cv::Point midT = toSubPt(0.0f, -9.5f);
        const cv::Point midB = toSubPt(0.0f, 9.5f);
        const cv::Point botL = toSubPt(-4.5f, 9.5f);
        const cv::Point botR = toSubPt(4.5f, 9.5f);

        cv::line(canvasBgr, topL, topR, cv::Scalar(255, 255, 255), thickBg, cv::LINE_AA, kShift);
        cv::line(canvasBgr, midT, midB, cv::Scalar(255, 255, 255), thickBg, cv::LINE_AA, kShift);
        cv::line(canvasBgr, botL, botR, cv::Scalar(255, 255, 255), thickBg, cv::LINE_AA, kShift);

        cv::line(canvasBgr, topL, topR, cv::Scalar(20, 23, 31), thickFg, cv::LINE_AA, kShift);
        cv::line(canvasBgr, midT, midB, cv::Scalar(20, 23, 31), thickFg, cv::LINE_AA, kShift);
        cv::line(canvasBgr, botL, botR, cv::Scalar(20, 23, 31), thickFg, cv::LINE_AA, kShift);
        return;
    }

    if (kind == StudioCursorKind::Cross) {
        const int thickBg = (std::max)(3, static_cast<int>(std::round(3.4f * scale)));
        const int thickFg = (std::max)(1, static_cast<int>(std::round(1.5f * scale)));
        cv::line(canvasBgr, toSubPt(-9.0f, 0.0f), toSubPt(9.0f, 0.0f), cv::Scalar(255, 255, 255), thickBg, cv::LINE_AA, kShift);
        cv::line(canvasBgr, toSubPt(0.0f, -9.0f), toSubPt(0.0f, 9.0f), cv::Scalar(255, 255, 255), thickBg, cv::LINE_AA, kShift);
        cv::line(canvasBgr, toSubPt(-9.0f, 0.0f), toSubPt(9.0f, 0.0f), cv::Scalar(20, 23, 31), thickFg, cv::LINE_AA, kShift);
        cv::line(canvasBgr, toSubPt(0.0f, -9.0f), toSubPt(0.0f, 9.0f), cv::Scalar(20, 23, 31), thickFg, cv::LINE_AA, kShift);
        return;
    }

    // 现代 macOS / Screen Studio 风格高精度矢量箭头指针（带柔和微投影 + 纯白外描边 + 曜石黑芯）
    static constexpr std::array<std::pair<float, float>, 7> kArrowShape = {{
        {0.0f, 0.0f},
        {0.0f, 19.2f},
        {4.8f, 14.8f},
        {8.2f, 22.0f},
        {11.1f, 20.6f},
        {7.6f, 13.4f},
        {13.8f, 13.4f},
    }};

    std::vector<cv::Point> shadowPoly;
    std::vector<cv::Point> mainPoly;
    shadowPoly.reserve(kArrowShape.size());
    mainPoly.reserve(kArrowShape.size());
    for (const auto& [px, py] : kArrowShape) {
        shadowPoly.push_back(toSubPt(px + 1.2f, py + 1.8f));
        mainPoly.push_back(toSubPt(px, py));
    }

    // 微阴影半透混合
    const int pad = static_cast<int>(std::ceil(28.0f * scale));
    const int sx0 = (std::max)(0, static_cast<int>(x) - 6);
    const int sy0 = (std::max)(0, static_cast<int>(y) - 6);
    const int sx1 = (std::min)(canvasBgr.cols, static_cast<int>(x) + pad);
    const int sy1 = (std::min)(canvasBgr.rows, static_cast<int>(y) + pad);
    if (sx1 > sx0 && sy1 > sy0) {
        cv::Mat roi = canvasBgr(cv::Rect(sx0, sy0, sx1 - sx0, sy1 - sy0));
        cv::Mat shLayer = roi.clone();
        std::vector<cv::Point> localShadow = shadowPoly;
        for (auto& pt : localShadow) {
            pt.x -= sx0 * static_cast<int>(kSub);
            pt.y -= sy0 * static_cast<int>(kSub);
        }
        cv::fillPoly(shLayer, std::vector<std::vector<cv::Point>>{localShadow}, cv::Scalar(10, 12, 18), cv::LINE_AA, kShift);
        cv::addWeighted(shLayer, 0.28, roi, 0.72, 0.0, roi);
    }

    const int borderThick = (std::max)(2, static_cast<int>(std::round(2.4f * scale)));
    cv::polylines(canvasBgr, std::vector<std::vector<cv::Point>>{mainPoly}, true,
                  cv::Scalar(255, 255, 255), borderThick, cv::LINE_AA, kShift);
    cv::fillPoly(canvasBgr, std::vector<std::vector<cv::Point>>{mainPoly},
                 kind == StudioCursorKind::Hand ? cv::Scalar(235, 99, 37) : cv::Scalar(18, 22, 32),
                 cv::LINE_AA, kShift);
}

cv::Mat StudioVideoCompositor::composeFrame(
    const cv::Mat& rawBgrOrBgra,
    const StudioEvaluatedFrame& eval,
    const StudioRenderConfig& config,
    cv::Size targetSize) {
    if (rawBgrOrBgra.empty()) return {};

    cv::Mat srcBgr;
    if (rawBgrOrBgra.channels() == 4) {
        cv::cvtColor(rawBgrOrBgra, srcBgr, cv::COLOR_BGRA2BGR);
    } else if (rawBgrOrBgra.channels() == 3) {
        srcBgr = rawBgrOrBgra;
    } else {
        cv::cvtColor(rawBgrOrBgra, srcBgr, cv::COLOR_GRAY2BGR);
    }

    const cv::Size outSize = (targetSize.width > 0 && targetSize.height > 0)
        ? resolveOutputCanvasSize(targetSize.width, targetSize.height)
        : resolveOutputCanvasSize(srcBgr.cols, srcBgr.rows);
    const int outW = outSize.width;
    const int outH = outSize.height;

    int padX = 0;
    int padY = 0;
    int innerW = outW;
    int innerH = outH;
    float cornerR = 0.0f;

    if (config.autoFrameEnabled) {
        const float safeRatio = std::clamp(config.paddingRatio, 0.02f, 0.16f);
        const int basePad = (std::max)(12, static_cast<int>(std::round((std::min)(outW, outH) * safeRatio)));
        const int availW = (std::max)(32, outW - basePad * 2);
        const int availH = (std::max)(32, outH - basePad * 2);
        const float scaleFit = (std::min)(
            static_cast<float>(availW) / static_cast<float>((std::max)(1, srcBgr.cols)),
            static_cast<float>(availH) / static_cast<float>((std::max)(1, srcBgr.rows)));
        innerW = (std::max)(16, static_cast<int>(std::round(srcBgr.cols * scaleFit)));
        innerH = (std::max)(16, static_cast<int>(std::round(srcBgr.rows * scaleFit)));
        padX = (outW - innerW) / 2;
        padY = (outH - innerH) / 2;
        cornerR = std::clamp(config.cornerRadius * (static_cast<float>(outH) / 1080.0f), 4.0f, 36.0f);
    }

    ensureBackdropCache(outW, outH, config.autoFrameEnabled, config.backgroundTheme, padX, padY, innerW, innerH, cornerR);
    cv::Mat canvas = m_cachedBackdropBgr.clone();

    // 1. 按当前弹簧摄像机裁剪窗口采样并缩放到 innerW x innerH
    // 注意：当输入为降采样预览帧时，按比例换算 eval.camera.crop* 坐标；若 camera 未初始化则展示全幅画面
    float effCropX = 0.0f;
    float effCropY = 0.0f;
    float effCropW = static_cast<float>(srcBgr.cols);
    float effCropH = static_cast<float>(srcBgr.rows);
    if (eval.camera.cropW > 0.5f && eval.camera.cropH > 0.5f && eval.camera.zoom >= 0.99f) {
        const float scaleRatioX = static_cast<float>(srcBgr.cols) /
            (std::max)(1.0f, eval.camera.cropW * eval.camera.zoom);
        const float scaleRatioY = static_cast<float>(srcBgr.rows) /
            (std::max)(1.0f, eval.camera.cropH * eval.camera.zoom);

        effCropX = std::clamp(eval.camera.cropX * scaleRatioX, 0.0f, static_cast<float>(srcBgr.cols - 1));
        effCropY = std::clamp(eval.camera.cropY * scaleRatioY, 0.0f, static_cast<float>(srcBgr.rows - 1));
        effCropW = std::clamp(eval.camera.cropW * scaleRatioX, 1.0f, static_cast<float>(srcBgr.cols) - effCropX);
        effCropH = std::clamp(eval.camera.cropH * scaleRatioY, 1.0f, static_cast<float>(srcBgr.rows) - effCropY);
    }

    const int ix = static_cast<int>(std::floor(effCropX));
    const int iy = static_cast<int>(std::floor(effCropY));
    const int iw = std::clamp(static_cast<int>(std::round(effCropW)), 1, srcBgr.cols - ix);
    const int ih = std::clamp(static_cast<int>(std::round(effCropH)), 1, srcBgr.rows - iy);

    cv::Mat zoomedInner;
    cv::resize(srcBgr(cv::Rect(ix, iy, iw, ih)), zoomedInner, cv::Size(innerW, innerH), 0.0, 0.0, cv::INTER_LINEAR);

    // 2. 将内嵌画面圆角抗锯齿合成到壁纸画布中央
    cv::Mat dstInner = canvas(cv::Rect(padX, padY, innerW, innerH));
    if (!config.autoFrameEnabled || cornerR <= 0.5f) {
        zoomedInner.copyTo(dstInner);
    } else {
        zoomedInner.copyTo(dstInner);
        const float r = (std::min)({cornerR, innerW * 0.5f, innerH * 0.5f});
        const int intR = static_cast<int>(std::ceil(r));
        auto blendCorner = [&](int sx, int ex, int sy, int ey, float cx, float cy) {
            for (int y = sy; y < ey; ++y) {
                uint8_t* dstRow = dstInner.ptr<uint8_t>(y);
                const uint8_t* bgRow = m_cachedBackdropBgr.ptr<uint8_t>(padY + y) + padX * 3;
                const uint8_t* fgRow = zoomedInner.ptr<uint8_t>(y);
                const float py = static_cast<float>(y) + 0.5f;
                for (int x = sx; x < ex; ++x) {
                    const float px = static_cast<float>(x) + 0.5f;
                    const float d = std::hypot(px - cx, py - cy);
                    if (d > r - 0.5f) {
                        const float cov = std::clamp(r + 0.5f - d, 0.0f, 1.0f);
                        const float inv = 1.0f - cov;
                        dstRow[x * 3 + 0] = static_cast<uint8_t>(fgRow[x * 3 + 0] * cov + bgRow[x * 3 + 0] * inv);
                        dstRow[x * 3 + 1] = static_cast<uint8_t>(fgRow[x * 3 + 1] * cov + bgRow[x * 3 + 1] * inv);
                        dstRow[x * 3 + 2] = static_cast<uint8_t>(fgRow[x * 3 + 2] * cov + bgRow[x * 3 + 2] * inv);
                    }
                }
            }
        };
        blendCorner(0, intR, 0, intR, r, r);
        blendCorner(innerW - intR, innerW, 0, intR, innerW - r, r);
        blendCorner(0, intR, innerH - intR, innerH, r, innerH - r);
        blendCorner(innerW - intR, innerW, innerH - intR, innerH, innerW - r, innerH - r);
    }

    // 3. 在放大后的最终视口顶层重绘平滑超清矢量光标（绝不随画面放大产生像素模糊！）
    if (config.smoothCursorEnabled && eval.cursorKind != StudioCursorKind::Hidden) {
        const float normX = (eval.smoothCursorX - eval.camera.cropX) / (std::max)(1.0f, eval.camera.cropW);
        const float normY = (eval.smoothCursorY - eval.camera.cropY) / (std::max)(1.0f, eval.camera.cropH);
        if (normX >= -0.02f && normX <= 1.02f && normY >= -0.02f && normY <= 1.02f) {
            const float curCanvasX = static_cast<float>(padX) + normX * static_cast<float>(innerW);
            const float curCanvasY = static_cast<float>(padY) + normY * static_cast<float>(innerH);
            const float dpiRatio = std::clamp(static_cast<float>(outH) / 900.0f, 0.7f, 2.2f);
            drawCrispVectorCursor(
                canvas,
                curCanvasX,
                curCanvasY,
                eval.cursorKind,
                config.cursorScale * eval.clickScale * dpiRatio,
                eval.rippleProgress,
                eval.rippleIsRight);
        }
    }

    // 4. 叠加时间轴自动对齐的底部胶囊字幕
    if (config.autoCaptionsEnabled && !eval.activeCaption.empty()) {
        ensureCaptionOverlayCache(eval.activeCaption, outW, outH);
        if (!m_cachedCaptionBgra.empty()) {
            alphaBlendPremultipliedBgraOntoBgr(
                m_cachedCaptionBgra, canvas, m_cachedCaptionDrawX, m_cachedCaptionDrawY);
        }
    }

    return canvas;
}

bool StudioVideoCompositor::extractPreviewDeck(
    const std::string& rawInputPathUtf8,
    int maxPreviewWidth,
    int maxDeckFrames,
    std::vector<cv::Mat>& outFramesBgr,
    int& outOrigWidth,
    int& outOrigHeight,
    double& outDurationSec) {
    outFramesBgr.clear();
    outOrigWidth = 0;
    outOrigHeight = 0;
    outDurationSec = 0.0;

    AVFormatContext* fmtCtx = nullptr;
    if (avformat_open_input(&fmtCtx, rawInputPathUtf8.c_str(), nullptr, nullptr) < 0 || !fmtCtx) {
        return false;
    }
    if (avformat_find_stream_info(fmtCtx, nullptr) < 0) {
        avformat_close_input(&fmtCtx);
        return false;
    }

    const AVCodec* vCodec = nullptr;
    const int vStreamIdx = av_find_best_stream(fmtCtx, AVMEDIA_TYPE_VIDEO, -1, -1, &vCodec, 0);
    if (vStreamIdx < 0 || !vCodec) {
        avformat_close_input(&fmtCtx);
        return false;
    }

    AVStream* vStream = fmtCtx->streams[vStreamIdx];
    outOrigWidth = vStream->codecpar->width;
    outOrigHeight = vStream->codecpar->height;
    if (fmtCtx->duration > 0) {
        outDurationSec = static_cast<double>(fmtCtx->duration) / static_cast<double>(AV_TIME_BASE);
    }

    AVCodecContext* decCtx = avcodec_alloc_context3(vCodec);
    if (!decCtx ||
        avcodec_parameters_to_context(decCtx, vStream->codecpar) < 0 ||
        avcodec_open2(decCtx, vCodec, nullptr) < 0) {
        if (decCtx) avcodec_free_context(&decCtx);
        avformat_close_input(&fmtCtx);
        return false;
    }

    const int safeMaxW = (std::max)(320, maxPreviewWidth);
    const float scale = (outOrigWidth > safeMaxW)
        ? (static_cast<float>(safeMaxW) / static_cast<float>(outOrigWidth))
        : 1.0f;
    const int prevW = (std::max)(64, static_cast<int>(std::round(outOrigWidth * scale)) & ~1);
    const int prevH = (std::max)(64, static_cast<int>(std::round(outOrigHeight * scale)) & ~1);

    SwsContext* sws = nullptr;
    AVPacket* pkt = av_packet_alloc();
    AVFrame* frm = av_frame_alloc();

    int64_t estTotalFrames = vStream->nb_frames;
    if (estTotalFrames <= 0 && outDurationSec > 0.0) {
        const double fps = (vStream->avg_frame_rate.den > 0)
            ? av_q2d(vStream->avg_frame_rate)
            : 30.0;
        estTotalFrames = static_cast<int64_t>(std::ceil(outDurationSec * (std::max)(1.0, fps)));
    }
    const int strideStep = (estTotalFrames > maxDeckFrames && maxDeckFrames > 0)
        ? static_cast<int>((estTotalFrames + maxDeckFrames - 1) / maxDeckFrames)
        : 1;

    int decodedIdx = 0;
    auto drainFrames = [&]() {
        while (avcodec_receive_frame(decCtx, frm) >= 0) {
            if ((decodedIdx % strideStep) == 0 &&
                (maxDeckFrames <= 0 || static_cast<int>(outFramesBgr.size()) < maxDeckFrames)) {
                if (!sws) {
                    sws = sws_getContext(
                        frm->width, frm->height, static_cast<AVPixelFormat>(frm->format),
                        prevW, prevH, AV_PIX_FMT_BGR24, SWS_BILINEAR, nullptr, nullptr, nullptr);
                }
                if (sws) {
                    cv::Mat bgr(prevH, prevW, CV_8UC3);
                    uint8_t* dstData[4] = {bgr.data, nullptr, nullptr, nullptr};
                    int dstLinesize[4] = {static_cast<int>(bgr.step[0]), 0, 0, 0};
                    sws_scale(sws, frm->data, frm->linesize, 0, frm->height, dstData, dstLinesize);
                    outFramesBgr.push_back(std::move(bgr));
                }
            }
            ++decodedIdx;
        }
    };

    while (av_read_frame(fmtCtx, pkt) >= 0) {
        if (pkt->stream_index == vStreamIdx) {
            if (avcodec_send_packet(decCtx, pkt) >= 0) {
                drainFrames();
            }
        }
        av_packet_unref(pkt);
        if (maxDeckFrames > 0 && static_cast<int>(outFramesBgr.size()) >= maxDeckFrames) {
            break;
        }
    }
    avcodec_send_packet(decCtx, nullptr);
    drainFrames();

    if (outDurationSec <= 0.0 && decodedIdx > 0) {
        const double fps = (vStream->avg_frame_rate.den > 0 && av_q2d(vStream->avg_frame_rate) > 0.5)
            ? av_q2d(vStream->avg_frame_rate)
            : 30.0;
        outDurationSec = static_cast<double>(decodedIdx) / fps;
    }

    if (sws) sws_freeContext(sws);
    av_frame_free(&frm);
    av_packet_free(&pkt);
    avcodec_free_context(&decCtx);
    avformat_close_input(&fmtCtx);
    return !outFramesBgr.empty();
}

bool exportRenderedVideoOnCleanMta(
    const std::string& rawInputPathUtf8,
    const std::string& finalOutputPathUtf8,
    const StudioTimelineData& timeline,
    const StudioRenderConfig& config,
    int bitrateMbps,
    const std::function<void(double progress01)>& onProgress,
    const std::atomic<bool>* cancelFlag) {
    if (rawInputPathUtf8.empty() || finalOutputPathUtf8.empty()) return false;

    const HRESULT hrCo = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    struct ComScopeGuard {
        bool needUninit = false;
        ~ComScopeGuard() {
            if (needUninit) CoUninitialize();
        }
    } comGuard{SUCCEEDED(hrCo)};

    AVFormatContext* inFmt = nullptr;
    if (avformat_open_input(&inFmt, rawInputPathUtf8.c_str(), nullptr, nullptr) < 0 || !inFmt) {
        return false;
    }
    if (avformat_find_stream_info(inFmt, nullptr) < 0) {
        avformat_close_input(&inFmt);
        return false;
    }

    const AVCodec* vDecCodec = nullptr;
    const int inVideoIdx = av_find_best_stream(inFmt, AVMEDIA_TYPE_VIDEO, -1, -1, &vDecCodec, 0);
    if (inVideoIdx < 0 || !vDecCodec) {
        avformat_close_input(&inFmt);
        return false;
    }
    const int inAudioIdx = av_find_best_stream(inFmt, AVMEDIA_TYPE_AUDIO, -1, -1, nullptr, 0);

    AVStream* inVStream = inFmt->streams[inVideoIdx];
    AVCodecContext* decCtx = avcodec_alloc_context3(vDecCodec);
    if (!decCtx ||
        avcodec_parameters_to_context(decCtx, inVStream->codecpar) < 0 ||
        avcodec_open2(decCtx, vDecCodec, nullptr) < 0) {
        if (decCtx) avcodec_free_context(&decCtx);
        avformat_close_input(&inFmt);
        return false;
    }

    const int outFps = std::clamp(timeline.fps > 0 ? timeline.fps : 30, 10, 120);
    const cv::Size outSize = StudioVideoCompositor::resolveOutputCanvasSize(decCtx->width, decCtx->height);

    const std::wstring finalWide = tools3000::core::WinUtils::utf8ToWstring(finalOutputPathUtf8);
    const std::wstring tempWide = finalWide + L".studio_tmp.mp4";
    const std::string tempUtf8 = tools3000::core::WinUtils::wstringToUtf8(tempWide);

    AVFormatContext* outFmt = nullptr;
    if (avformat_alloc_output_context2(&outFmt, nullptr, "mp4", tempUtf8.c_str()) < 0 || !outFmt) {
        avcodec_free_context(&decCtx);
        avformat_close_input(&inFmt);
        return false;
    }

    std::vector<const AVCodec*> encCandidates;
    wchar_t testModeEnv[8]{};
    const bool inUnitTestMode = GetEnvironmentVariableW(L"TOOLS3000_TEST_MODE", testModeEnv, 8) > 0 &&
                                testModeEnv[0] == L'1';
    if (inUnitTestMode) {
        for (const char* name : {"libx264", "libopenh264", "mpeg4", "h264_mf"}) {
            addCodecIfPresent(encCandidates, name);
        }
    } else {
        for (const char* name : {"h264_nvenc", "h264_qsv", "h264_amf", "h264_mf", "libx264", "libopenh264", "mpeg4"}) {
            addCodecIfPresent(encCandidates, name);
        }
    }

    AVCodecContext* encCtx = nullptr;
    const AVCodec* chosenEnc = nullptr;
    for (const auto* cand : encCandidates) {
        AVCodecContext* attempt = avcodec_alloc_context3(cand);
        if (!attempt) continue;
        const bool isMf = std::strstr(cand->name, "_mf") != nullptr;
        attempt->width = outSize.width;
        attempt->height = outSize.height;
        attempt->time_base = {1, outFps};
        attempt->framerate = {outFps, 1};
        attempt->gop_size = isMf ? 0 : outFps * 2;
        attempt->max_b_frames = 0;
        attempt->bit_rate = static_cast<int64_t>(std::clamp(bitrateMbps, 2, 80)) * 1'000'000LL;
        attempt->pix_fmt = selectStudioEncoderPixFmt(cand);
        if (outFmt->oformat->flags & AVFMT_GLOBALHEADER) {
            attempt->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;
        }
        if (std::strcmp(cand->name, "libx264") == 0) {
            av_opt_set(attempt->priv_data, "preset", "veryfast", 0);
        }
        if (avcodec_open2(attempt, cand, nullptr) >= 0) {
            encCtx = attempt;
            chosenEnc = cand;
            break;
        }
        avcodec_free_context(&attempt);
    }

    if (!encCtx || !chosenEnc) {
        avformat_free_context(outFmt);
        avcodec_free_context(&decCtx);
        avformat_close_input(&inFmt);
        return false;
    }

    AVStream* outVStream = avformat_new_stream(outFmt, chosenEnc);
    if (!outVStream || avcodec_parameters_from_context(outVStream->codecpar, encCtx) < 0) {
        avcodec_free_context(&encCtx);
        avformat_free_context(outFmt);
        avcodec_free_context(&decCtx);
        avformat_close_input(&inFmt);
        return false;
    }
    outVStream->time_base = encCtx->time_base;

    AVStream* outAStream = nullptr;
    if (inAudioIdx >= 0) {
        AVStream* inAStream = inFmt->streams[inAudioIdx];
        if (inAStream->codecpar->codec_id == AV_CODEC_ID_AAC ||
            inAStream->codecpar->codec_id == AV_CODEC_ID_MP3) {
            outAStream = avformat_new_stream(outFmt, nullptr);
            if (outAStream && avcodec_parameters_copy(outAStream->codecpar, inAStream->codecpar) >= 0) {
                outAStream->codecpar->codec_tag = 0;
                outAStream->time_base = inAStream->time_base;
            } else {
                outAStream = nullptr;
            }
        }
    }

    if (!(outFmt->oformat->flags & AVFMT_NOFILE)) {
        if (avio_open(&outFmt->pb, tempUtf8.c_str(), AVIO_FLAG_WRITE) < 0) {
            avcodec_free_context(&encCtx);
            avformat_free_context(outFmt);
            avcodec_free_context(&decCtx);
            avformat_close_input(&inFmt);
            return false;
        }
    }

    if (avformat_write_header(outFmt, nullptr) < 0) {
        if (outFmt->pb) avio_closep(&outFmt->pb);
        avcodec_free_context(&encCtx);
        avformat_free_context(outFmt);
        avcodec_free_context(&decCtx);
        avformat_close_input(&inFmt);
        std::error_code ec;
        std::filesystem::remove(tempWide, ec);
        return false;
    }

    const int estTotalFrames = (std::max)(
        1, static_cast<int>(std::ceil((timeline.durationMs / 1000.0) * outFps)) + outFps * 2);
    StudioTimelineData bakedTimeline = timeline;
    if (bakedTimeline.sourceWidth <= 0) bakedTimeline.sourceWidth = decCtx->width;
    if (bakedTimeline.sourceHeight <= 0) bakedTimeline.sourceHeight = decCtx->height;
    const auto evaluatedFrames = StudioTimelineAnalyzer::bakeEvaluatedFrames(
        bakedTimeline, config, outFps, estTotalFrames);

    StudioVideoCompositor compositor;
    SwsContext* toBgrSws = nullptr;
    SwsContext* fromBgrSws = sws_getContext(
        outSize.width, outSize.height, AV_PIX_FMT_BGR24,
        outSize.width, outSize.height, encCtx->pix_fmt,
        SWS_BILINEAR, nullptr, nullptr, nullptr);

    AVPacket* inPkt = av_packet_alloc();
    AVPacket* outPkt = av_packet_alloc();
    AVFrame* decFrame = av_frame_alloc();
    AVFrame* encFrame = av_frame_alloc();
    encFrame->format = encCtx->pix_fmt;
    encFrame->width = outSize.width;
    encFrame->height = outSize.height;
    av_frame_get_buffer(encFrame, 0);

    int64_t encodedCount = 0;
    bool ok = true;

    auto drainEncoder = [&]() {
        while (avcodec_receive_packet(encCtx, outPkt) >= 0) {
            av_packet_rescale_ts(outPkt, encCtx->time_base, outVStream->time_base);
            outPkt->stream_index = outVStream->index;
            if (av_interleaved_write_frame(outFmt, outPkt) < 0) {
                ok = false;
            }
            av_packet_unref(outPkt);
        }
    };

    auto processDecodedFrames = [&]() {
        while (ok && avcodec_receive_frame(decCtx, decFrame) >= 0) {
            if (cancelFlag && cancelFlag->load(std::memory_order_acquire)) {
                ok = false;
                break;
            }
            if (!toBgrSws) {
                toBgrSws = sws_getContext(
                    decFrame->width, decFrame->height, static_cast<AVPixelFormat>(decFrame->format),
                    decFrame->width, decFrame->height, AV_PIX_FMT_BGR24,
                    SWS_BILINEAR, nullptr, nullptr, nullptr);
            }
            cv::Mat rawBgr(decFrame->height, decFrame->width, CV_8UC3);
            uint8_t* dstBgr[4] = {rawBgr.data, nullptr, nullptr, nullptr};
            int dstBgrStride[4] = {static_cast<int>(rawBgr.step[0]), 0, 0, 0};
            sws_scale(toBgrSws, decFrame->data, decFrame->linesize, 0, decFrame->height, dstBgr, dstBgrStride);

            const size_t evalIdx = (std::min)(
                static_cast<size_t>(encodedCount),
                evaluatedFrames.empty() ? size_t(0) : (evaluatedFrames.size() - 1));
            const StudioEvaluatedFrame fallbackEval{};
            const StudioEvaluatedFrame& eval = evaluatedFrames.empty() ? fallbackEval : evaluatedFrames[evalIdx];

            cv::Mat composed = compositor.composeFrame(rawBgr, eval, config, outSize);
            if (av_frame_make_writable(encFrame) < 0) {
                ok = false;
                break;
            }
            const uint8_t* srcComposed[4] = {composed.data, nullptr, nullptr, nullptr};
            int srcComposedStride[4] = {static_cast<int>(composed.step[0]), 0, 0, 0};
            sws_scale(fromBgrSws, srcComposed, srcComposedStride, 0, outSize.height, encFrame->data, encFrame->linesize);

            encFrame->pts = encodedCount++;
            if (avcodec_send_frame(encCtx, encFrame) < 0) {
                ok = false;
                break;
            }
            drainEncoder();

            if (onProgress) {
                const double denom = (std::max)(1.0, (timeline.durationMs / 1000.0) * outFps);
                onProgress(std::clamp(static_cast<double>(encodedCount) / denom, 0.0, 0.99));
            }
        }
    };

    while (ok && av_read_frame(inFmt, inPkt) >= 0) {
        if (cancelFlag && cancelFlag->load(std::memory_order_acquire)) {
            av_packet_unref(inPkt);
            ok = false;
            break;
        }
        if (inPkt->stream_index == inVideoIdx) {
            if (avcodec_send_packet(decCtx, inPkt) >= 0) {
                processDecodedFrames();
            }
        } else if (outAStream && inPkt->stream_index == inAudioIdx) {
            av_packet_rescale_ts(inPkt, inFmt->streams[inAudioIdx]->time_base, outAStream->time_base);
            inPkt->stream_index = outAStream->index;
            inPkt->pos = -1;
            av_interleaved_write_frame(outFmt, inPkt);
        }
        av_packet_unref(inPkt);
    }

    if (ok) {
        avcodec_send_packet(decCtx, nullptr);
        processDecodedFrames();
        avcodec_send_frame(encCtx, nullptr);
        drainEncoder();
        if (av_write_trailer(outFmt) < 0) {
            ok = false;
        }
    }

    if (toBgrSws) sws_freeContext(toBgrSws);
    if (fromBgrSws) sws_freeContext(fromBgrSws);
    av_frame_free(&decFrame);
    av_frame_free(&encFrame);
    av_packet_free(&inPkt);
    av_packet_free(&outPkt);
    if (outFmt->pb) avio_closep(&outFmt->pb);
    avcodec_free_context(&encCtx);
    avformat_free_context(outFmt);
    avcodec_free_context(&decCtx);
    avformat_close_input(&inFmt);

    std::error_code ec;
    if (!ok || encodedCount == 0) {
        std::filesystem::remove(tempWide, ec);
        return false;
    }

    if (!MoveFileExW(tempWide.c_str(), finalWide.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        std::filesystem::remove(tempWide, ec);
        return false;
    }

    if (config.exportSrtSidecar && config.autoCaptionsEnabled && !timeline.captions.empty()) {
        std::filesystem::path srtPath(finalWide);
        srtPath.replace_extension(L".srt");
        StudioTimelineAnalyzer::exportSrtFile(srtPath.wstring(), timeline.captions);
    }

    if (onProgress) onProgress(1.0);
    return true;
}

bool StudioVideoCompositor::exportRenderedVideo(
    const std::string& rawInputPathUtf8,
    const std::string& finalOutputPathUtf8,
    const StudioTimelineData& timeline,
    const StudioRenderConfig& config,
    int bitrateMbps,
    const std::function<void(double progress01)>& onProgress,
    const std::atomic<bool>* cancelFlag) {
    if (rawInputPathUtf8.empty() || finalOutputPathUtf8.empty()) return false;

    APTTYPE aptType = APTTYPE_CURRENT;
    APTTYPEQUALIFIER aptQual = APTTYPEQUALIFIER_NONE;
    const bool onSta = SUCCEEDED(CoGetApartmentType(&aptType, &aptQual)) &&
                       (aptType == APTTYPE_STA || aptType == APTTYPE_MAINSTA);
    if (onSta) {
        auto ok = std::make_shared<bool>(false);
        std::thread mtaWorker([&, ok]() {
            *ok = exportRenderedVideoOnCleanMta(
                rawInputPathUtf8, finalOutputPathUtf8, timeline, config, bitrateMbps, onProgress, cancelFlag);
        });
        tools3000::core::joinWorkerWhilePumpingSentMessages(mtaWorker, 600000);
        return *ok;
    }

    return exportRenderedVideoOnCleanMta(
        rawInputPathUtf8, finalOutputPathUtf8, timeline, config, bitrateMbps, onProgress, cancelFlag);
}

} // namespace tools3000::capture
