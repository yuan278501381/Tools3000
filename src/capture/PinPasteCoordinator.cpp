// ─────────────────────────────────────────────────────────────────────────────
// PinPasteCoordinator.cpp — 世界级连续历史贴图会话调度中枢与微晶字签渲染实现
//
// 版权声明: Copyright (c) 2026 Yy1 (yuan278501381). All rights reserved.
// ─────────────────────────────────────────────────────────────────────────────

#include "capture/PinPasteCoordinator.h"
#include "capture/PinSpawnCalculator.h"
#include "capture/CaptureHistory.h"
#include "capture/ClipboardUtils.h"
#include "core/logger/Logger.h"
#include "core/utils/DpiUtils.h"
#include "core/utils/DWriteFontUtils.h"
#include <d2d1.h>
#include <dwrite.h>
#include <wrl/client.h>
#include <algorithm>
#include <cmath>
#include <cstring>
#include <cwctype>
#include <format>
#include <functional>
#include <opencv2/imgproc.hpp>

namespace tools3000::capture {

namespace {

using Microsoft::WRL::ComPtr;

float queryActiveMonitorScale() {
    wchar_t testMode[8]{};
    if (GetEnvironmentVariableW(L"TOOLS3000_TEST_MODE", testMode, 8) > 0 && testMode[0] == L'1') {
        return 1.0f;
    }
    POINT pt{};
    if (GetCursorPos(&pt)) {
        HMONITOR mon = MonitorFromPoint(pt, MONITOR_DEFAULTTONEAREST);
        return std::clamp(tools3000::core::dpi::scaleForMonitor(mon), 1.0f, 3.5f);
    }
    return 1.0f;
}

std::wstring trimClipboardSourceText(const std::wstring& raw) {
    const size_t firstNonNewline = raw.find_first_not_of(L"\r\n");
    if (firstNonNewline == std::wstring::npos) return {};
    const size_t lastNonWs = raw.find_last_not_of(L" \t\r\n");
    if (lastNonWs == std::wstring::npos || lastNonWs < firstNonNewline) return {};
    return raw.substr(firstNonNewline, lastNonWs - firstNonNewline + 1);
}

std::wstring normalizeDisplayText(const std::wstring& trimmed, bool& outTruncated) {
    outTruncated = false;
    std::wstring out;
    out.reserve((std::min)(trimmed.size() + 32, static_cast<size_t>(2048)));

    constexpr int kMaxDisplayLines = 26;
    constexpr size_t kMaxDisplayChars = 1500;
    int lineCount = 1;

    for (size_t i = 0; i < trimmed.size(); ++i) {
        const wchar_t ch = trimmed[i];
        if (ch == L'\r') {
            if (i + 1 < trimmed.size() && trimmed[i + 1] == L'\n') {
                ++i;
            }
            if (lineCount >= kMaxDisplayLines) {
                outTruncated = true;
                break;
            }
            out.push_back(L'\n');
            ++lineCount;
        } else if (ch == L'\n') {
            if (lineCount >= kMaxDisplayLines) {
                outTruncated = true;
                break;
            }
            out.push_back(L'\n');
            ++lineCount;
        } else if (ch == L'\t') {
            out.append(L"    ");
        } else {
            out.push_back(ch);
        }

        if (out.size() >= kMaxDisplayChars) {
            outTruncated = (i + 1 < trimmed.size());
            break;
        }
    }

    if (outTruncated) {
        out.append(L" ...");
    }
    return out;
}

bool shouldUseMonospaceForNote(const std::wstring& text) noexcept {
    bool hasCjk = false;
    bool hasDigitOrTokenSym = false;
    int spaceCount = 0;
    bool isMultiLine = false;

    for (wchar_t ch : text) {
        if (ch >= 0x2E80) {
            hasCjk = true;
            break;
        }
        if (ch == L'\n') {
            isMultiLine = true;
        } else if (ch == L' ') {
            ++spaceCount;
        } else if (std::iswdigit(ch) ||
                   ch == L'@' || ch == L'#' || ch == L'$' || ch == L'%' ||
                   ch == L'&' || ch == L'*' || ch == L'_' || ch == L'=' ||
                   ch == L'+' || ch == L'/' || ch == L'\\' || ch == L':' ||
                   ch == L';' || ch == L'{' || ch == L'}' || ch == L'[' ||
                   ch == L']' || ch == L'<' || ch == L'>' || ch == L'!' ||
                   ch == L'~' || ch == L'`' || ch == L'|') {
            hasDigitOrTokenSym = true;
        }
    }

    if (hasCjk) return false;
    if (!isMultiLine) {
        // 单行凭证、密码 (如 Hycan@!0798)、API Key、IP、命令参数、短标识符优先使用现代等宽字体，精准区分 0/O 与 1/l/I
        if (spaceCount <= 1 || hasDigitOrTokenSym) return true;
        return false;
    }

    return hasDigitOrTokenSym ||
           text.find(L"  ") != std::wstring::npos ||
           text.find(L"def ") != std::wstring::npos ||
           text.find(L"const ") != std::wstring::npos ||
           text.find(L"function") != std::wstring::npos ||
           text.find(L"import ") != std::wstring::npos ||
           text.find(L"class ") != std::wstring::npos ||
           text.find(L"return ") != std::wstring::npos;
}

bool renderD2DBitmapToMat(int w, int h, const std::function<void(ID2D1DCRenderTarget*, ID2D1Factory*)>& drawFn, cv::Mat& outBgr) {
    if (w <= 0 || h <= 0) return false;

    BITMAPINFO bmi{};
    bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bmi.bmiHeader.biWidth = w;
    bmi.bmiHeader.biHeight = -h; // top-down
    bmi.bmiHeader.biPlanes = 1;
    bmi.bmiHeader.biBitCount = 32;
    bmi.bmiHeader.biCompression = BI_RGB;

    HDC screenDc = GetDC(nullptr);
    HDC memDc = CreateCompatibleDC(screenDc);
    void* bits = nullptr;
    HBITMAP dib = CreateDIBSection(screenDc, &bmi, DIB_RGB_COLORS, &bits, nullptr, 0);
    ReleaseDC(nullptr, screenDc);

    if (!memDc || !dib || !bits) {
        if (dib) DeleteObject(dib);
        if (memDc) DeleteDC(memDc);
        return false;
    }

    std::memset(bits, 0, static_cast<size_t>(w) * static_cast<size_t>(h) * 4);
    HGDIOBJ oldBm = SelectObject(memDc, dib);

    bool ok = false;
    ComPtr<ID2D1Factory> d2dFactory;
    if (SUCCEEDED(D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, d2dFactory.GetAddressOf())) && d2dFactory) {
        D2D1_RENDER_TARGET_PROPERTIES rtProps = D2D1::RenderTargetProperties(
            D2D1_RENDER_TARGET_TYPE_DEFAULT,
            D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED),
            96.0f, 96.0f);
        ComPtr<ID2D1DCRenderTarget> rt;
        RECT bindRc{0, 0, w, h};
        if (SUCCEEDED(d2dFactory->CreateDCRenderTarget(&rtProps, rt.GetAddressOf())) &&
            rt && SUCCEEDED(rt->BindDC(memDc, &bindRc))) {
            rt->SetDpi(96.0f, 96.0f);
            rt->SetAntialiasMode(D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);
            rt->SetTextAntialiasMode(D2D1_TEXT_ANTIALIAS_MODE_CLEARTYPE);

            rt->BeginDraw();
            drawFn(rt.Get(), d2dFactory.Get());
            ok = SUCCEEDED(rt->EndDraw());
        }
    }

    if (ok) {
        GdiFlush();
        cv::Mat bgra(h, w, CV_8UC4, bits);
        cv::cvtColor(bgra, outBgr, cv::COLOR_BGRA2BGR);
    }

    SelectObject(memDc, oldBm);
    DeleteObject(dib);
    DeleteDC(memDc);
    return ok && !outBgr.empty();
}

} // namespace

bool PinPasteCoordinator::parseHexColor(const std::wstring& in, cv::Scalar& bgr, std::wstring& label) {
    size_t a = in.find_first_not_of(L" \t\r\n");
    size_t b = in.find_last_not_of(L" \t\r\n");
    if (a == std::wstring::npos) return false;
    std::wstring s = in.substr(a, b - a + 1);
    if (s.size() < 4 || s[0] != L'#') return false;
    std::wstring hex = s.substr(1);
    if (hex.size() != 3 && hex.size() != 6) return false;
    for (wchar_t c : hex) if (!std::iswxdigit(c)) return false;

    auto hv = [](wchar_t c) -> int {
        c = std::towlower(c);
        return (c <= L'9') ? (c - L'0') : (c - L'a' + 10);
    };
    int r, g, bl;
    if (hex.size() == 3) { r = hv(hex[0]) * 17; g = hv(hex[1]) * 17; bl = hv(hex[2]) * 17; }
    else { r = hv(hex[0]) * 16 + hv(hex[1]); g = hv(hex[2]) * 16 + hv(hex[3]); bl = hv(hex[4]) * 16 + hv(hex[5]); }
    bgr = cv::Scalar(bl, g, r);
    label = s;
    return true;
}

cv::Mat PinPasteCoordinator::renderColorSwatch(const cv::Scalar& bgr, const std::wstring& label, float dpiScaleOverride) {
    const float s = (dpiScaleOverride > 0.05f)
        ? std::clamp(dpiScaleOverride, 1.0f, 3.5f)
        : queryActiveMonitorScale();
    const int w = (std::max)(1, static_cast<int>(std::lround(180.0f * s)));
    const int h = (std::max)(1, static_cast<int>(std::lround(130.0f * s)));
    const int bVal = std::clamp(static_cast<int>( std::lround(bgr[0]) ), 0, 255);
    const int gVal = std::clamp(static_cast<int>( std::lround(bgr[1]) ), 0, 255);
    const int rVal = std::clamp(static_cast<int>( std::lround(bgr[2]) ), 0, 255);

    std::wstring upperHex = label;
    for (auto& ch : upperHex) ch = static_cast<wchar_t>(std::towupper(ch));
    const std::wstring rgbSub = std::format(L"RGB {}, {}, {}", rVal, gVal, bVal);

    ComPtr<IDWriteFactory> dwFactory;
    DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory),
                        reinterpret_cast<IUnknown**>(dwFactory.GetAddressOf()));

    cv::Mat result;
    const bool rendered = renderD2DBitmapToMat(w, h, [&](ID2D1DCRenderTarget* rt, [[maybe_unused]] ID2D1Factory* factory) {
        const float fw = static_cast<float>(w);
        const float fh = static_cast<float>(h);
        const float footerTop = std::round(82.0f * s);

        // 1. 纯净瓷白微晶底座
        ComPtr<ID2D1SolidColorBrush> baseBrush, swatchBrush, dividerBrush, borderBrush, dotRingBrush, hexBrush, subBrush;
        rt->CreateSolidColorBrush(D2D1::ColorF(0.988f, 0.992f, 0.996f, 1.0f), baseBrush.GetAddressOf());
        rt->CreateSolidColorBrush(D2D1::ColorF(rVal / 255.0f, gVal / 255.0f, bVal / 255.0f, 1.0f), swatchBrush.GetAddressOf());
        rt->CreateSolidColorBrush(D2D1::ColorF(0.796f, 0.835f, 0.882f, 0.85f), dividerBrush.GetAddressOf());
        rt->CreateSolidColorBrush(D2D1::ColorF(0.750f, 0.796f, 0.855f, 0.92f), borderBrush.GetAddressOf());
        rt->CreateSolidColorBrush(D2D1::ColorF(0.710f, 0.761f, 0.831f, 0.90f), dotRingBrush.GetAddressOf());
        rt->CreateSolidColorBrush(D2D1::ColorF(0.059f, 0.090f, 0.165f, 0.98f), hexBrush.GetAddressOf());
        rt->CreateSolidColorBrush(D2D1::ColorF(0.278f, 0.333f, 0.412f, 0.94f), subBrush.GetAddressOf());

        const D2D1_RECT_F fullRc = D2D1::RectF(0.0f, 0.0f, fw, fh);
        if (baseBrush) rt->FillRectangle(fullRc, baseBrush.Get());
        if (swatchBrush) rt->FillRectangle(D2D1::RectF(0.0f, 0.0f, fw, footerTop), swatchBrush.Get());
        if (dividerBrush) rt->DrawLine(D2D1::Point2F(0.0f, footerTop), D2D1::Point2F(fw, footerTop), dividerBrush.Get(), 1.0f * s);

        // 2. 底座左侧精致双环色珠 + 右侧 HEX / RGB 双行等宽铭牌
        const D2D1_ELLIPSE dot = D2D1::Ellipse(D2D1::Point2F(20.0f * s, footerTop + 24.0f * s), 7.0f * s, 7.0f * s);
        if (swatchBrush) rt->FillEllipse(dot, swatchBrush.Get());
        if (dotRingBrush) rt->DrawEllipse(dot, dotRingBrush.Get(), 1.4f * s);

        if (dwFactory) {
            const wchar_t* monoFont = tools3000::core::resolveInstalledMonoFontFamily(dwFactory.Get());
            ComPtr<IDWriteTextFormat> hexFmt, subFmt;
            if (SUCCEEDED(dwFactory->CreateTextFormat(
                    monoFont, nullptr, DWRITE_FONT_WEIGHT_SEMI_BOLD,
                    DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL,
                    13.5f * s, L"en-US", hexFmt.GetAddressOf())) && hexFmt) {
                hexFmt->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
                hexFmt->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
                if (hexBrush) {
                    rt->DrawTextW(upperHex.c_str(), static_cast<UINT32>(upperHex.size()),
                                  hexFmt.Get(), D2D1::RectF(35.0f * s, footerTop + 5.0f * s, fw - 10.0f * s, footerTop + 25.0f * s),
                                  hexBrush.Get(), D2D1_DRAW_TEXT_OPTIONS_CLIP);
                }
            }
            if (SUCCEEDED(dwFactory->CreateTextFormat(
                    monoFont, nullptr, DWRITE_FONT_WEIGHT_MEDIUM,
                    DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL,
                    11.0f * s, L"en-US", subFmt.GetAddressOf())) && subFmt) {
                subFmt->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
                subFmt->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
                if (subBrush) {
                    rt->DrawTextW(rgbSub.c_str(), static_cast<UINT32>(rgbSub.size()),
                                  subFmt.Get(), D2D1::RectF(35.0f * s, footerTop + 25.0f * s, fw - 8.0f * s, fh - 5.0f * s),
                                  subBrush.Get(), D2D1_DRAW_TEXT_OPTIONS_CLIP);
                }
            }
        }

        // 3. 10px 同心圆角云母浅灰细边
        if (borderBrush) {
            const float inset = 0.75f * s;
            const float radius = 9.25f * s;
            const D2D1_ROUNDED_RECT innerBorder = D2D1::RoundedRect(
                D2D1::RectF(inset, inset, fw - inset, fh - inset), radius, radius);
            rt->DrawRoundedRectangle(innerBorder, borderBrush.Get(), 1.2f * s);
        }
    }, result);

    if (rendered) return result;

    // 极端无 D2D 环境保底
    cv::Mat fallback(h, w, CV_8UC3, bgr);
    return fallback;
}

cv::Mat PinPasteCoordinator::renderTextToImage(const std::wstring& text, float dpiScaleOverride) {
    if (text.find_first_not_of(L" \t\r\n") == std::wstring::npos) {
        return {};
    }

    const std::wstring cleanSource = trimClipboardSourceText(text);
    if (cleanSource.empty()) {
        return {};
    }

    bool truncated = false;
    const std::wstring displayText = normalizeDisplayText(cleanSource, truncated);
    if (displayText.empty()) {
        return {};
    }

    const float s = (dpiScaleOverride > 0.05f)
        ? std::clamp(dpiScaleOverride, 1.0f, 3.5f)
        : queryActiveMonitorScale();

    ComPtr<IDWriteFactory> dwFactory;
    if (FAILED(DWriteCreateFactory(
            DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory),
            reinterpret_cast<IUnknown**>(dwFactory.GetAddressOf()))) || !dwFactory) {
        return {};
    }

    const bool useMono = shouldUseMonospaceForNote(displayText);
    const bool hasExplicitNewline = (displayText.find(L'\n') != std::wstring::npos);
    const wchar_t* fontFamily = useMono
        ? tools3000::core::resolveInstalledMonoFontFamily(dwFactory.Get())
        : tools3000::core::resolveInstalledUiFontFamily(dwFactory.Get());

    const float fontPx = (hasExplicitNewline ? 14.5f : 15.5f) * s;
    const DWRITE_FONT_WEIGHT fontWeight = hasExplicitNewline
        ? DWRITE_FONT_WEIGHT_MEDIUM
        : DWRITE_FONT_WEIGHT_SEMI_BOLD;

    ComPtr<IDWriteTextFormat> textFormat;
    if (FAILED(dwFactory->CreateTextFormat(
            fontFamily, nullptr, fontWeight,
            DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL,
            fontPx, L"zh-CN", textFormat.GetAddressOf())) || !textFormat) {
        return {};
    }

    textFormat->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
    textFormat->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_NEAR);
    textFormat->SetWordWrapping(DWRITE_WORD_WRAPPING_WRAP);
    if (hasExplicitNewline) {
        textFormat->SetLineSpacing(DWRITE_LINE_SPACING_METHOD_UNIFORM, 23.0f * s, 18.0f * s);
    }

    const float maxTextWidth = (hasExplicitNewline ? 640.0f : 560.0f) * s;
    ComPtr<IDWriteTextLayout> textLayout;
    if (FAILED(dwFactory->CreateTextLayout(
            displayText.c_str(), static_cast<UINT32>(displayText.size()),
            textFormat.Get(), maxTextWidth, 4096.0f * s,
            textLayout.GetAddressOf())) || !textLayout) {
        return {};
    }

    DWRITE_TEXT_METRICS metrics{};
    if (FAILED(textLayout->GetMetrics(&metrics))) {
        return {};
    }

    const bool effectiveMultiLine = hasExplicitNewline || (metrics.lineCount > 1);
    const float padLeft   = (effectiveMultiLine ? 22.0f : 21.0f) * s;
    const float padRight  = (effectiveMultiLine ? 18.0f : 16.0f) * s;
    const float padTop    = (effectiveMultiLine ? 13.0f : 11.0f) * s;
    const float padBottom = (effectiveMultiLine ? 13.0f : 11.0f) * s;

    const float measuredW = std::ceil((std::max)(metrics.widthIncludingTrailingWhitespace, metrics.width)) + 2.0f * s;
    const float measuredH = std::ceil(metrics.height);

    const int minCardW = static_cast<int>(std::lround((effectiveMultiLine ? 184.0f : 116.0f) * s));
    const int maxCardW = static_cast<int>(std::ceil(maxTextWidth + padLeft + padRight));
    const int minCardH = static_cast<int>(std::lround((effectiveMultiLine ? 58.0f : 42.0f) * s));
    const int maxCardH = static_cast<int>(std::lround(1800.0f * s));

    const int w = std::clamp(static_cast<int>(std::ceil(measuredW + padLeft + padRight)), minCardW, maxCardW);
    const int h = std::clamp(static_cast<int>(std::ceil(measuredH + padTop + padBottom)), minCardH, maxCardH);

    const float layoutBoxW = (std::max)(16.0f, static_cast<float>(w) - padLeft - padRight);
    const float layoutBoxH = (std::max)(16.0f, static_cast<float>(h) - padTop - padBottom);
    textLayout->SetMaxWidth(layoutBoxW);
    textLayout->SetMaxHeight(layoutBoxH);
    if (!effectiveMultiLine) {
        textLayout->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
    }

    cv::Mat result;
    renderD2DBitmapToMat(w, h, [&](ID2D1DCRenderTarget* rt, [[maybe_unused]] ID2D1Factory* factory) {
        const float fw = static_cast<float>(w);
        const float fh = static_cast<float>(h);
        const D2D1_RECT_F fullRc = D2D1::RectF(0.0f, 0.0f, fw, fh);

        // 1. 纯净亮白微线性渐变底舱 (#FFFFFF -> #F8FAFC，零视觉负担，干净清爽)
        const D2D1_GRADIENT_STOP bgStops[2] = {
            {0.0f, D2D1::ColorF(1.0f, 1.0f, 1.0f, 1.0f)},
            {1.0f, D2D1::ColorF(0.973f, 0.980f, 0.988f, 1.0f)},
        };
        ComPtr<ID2D1GradientStopCollection> bgStopCol;
        ComPtr<ID2D1LinearGradientBrush> bgBrush;
        if (SUCCEEDED(rt->CreateGradientStopCollection(bgStops, 2, bgStopCol.GetAddressOf())) && bgStopCol) {
            rt->CreateLinearGradientBrush(
                D2D1::LinearGradientBrushProperties(D2D1::Point2F(0.0f, 0.0f), D2D1::Point2F(0.0f, fh)),
                bgStopCol.Get(), bgBrush.GetAddressOf());
        }
        if (bgBrush) {
            rt->FillRectangle(fullRc, bgBrush.Get());
        } else {
            ComPtr<ID2D1SolidColorBrush> solidBg;
            rt->CreateSolidColorBrush(D2D1::ColorF(1.0f, 1.0f, 1.0f, 1.0f), solidBg.GetAddressOf());
            if (solidBg) rt->FillRectangle(fullRc, solidBg.Get());
        }

        // 2. 左侧圆头清爽科技蓝书签导条 (#3B82F6 -> #60A5FA)
        const float barW = 3.0f * s;
        const float barX = 9.0f * s;
        const float barTop = effectiveMultiLine ? (13.0f * s) : ((fh - 16.0f * s) * 0.5f);
        const float barBottom = effectiveMultiLine ? (fh - 13.0f * s) : ((fh + 16.0f * s) * 0.5f);
        if (barBottom > barTop + 4.0f * s) {
            const D2D1_GRADIENT_STOP barStops[2] = {
                {0.0f, D2D1::ColorF(0.231f, 0.510f, 0.965f, 0.96f)},
                {1.0f, D2D1::ColorF(0.376f, 0.647f, 0.980f, 0.92f)},
            };
            ComPtr<ID2D1GradientStopCollection> barCol;
            ComPtr<ID2D1LinearGradientBrush> barBrush;
            if (SUCCEEDED(rt->CreateGradientStopCollection(barStops, 2, barCol.GetAddressOf())) && barCol) {
                rt->CreateLinearGradientBrush(
                    D2D1::LinearGradientBrushProperties(D2D1::Point2F(barX, barTop), D2D1::Point2F(barX, barBottom)),
                    barCol.Get(), barBrush.GetAddressOf());
                if (barBrush) {
                    const D2D1_ROUNDED_RECT barPill = D2D1::RoundedRect(
                        D2D1::RectF(barX, barTop, barX + barW, barBottom),
                        barW * 0.5f, barW * 0.5f);
                    rt->FillRoundedRectangle(barPill, barBrush.Get());
                }
            }
        }

        // 3. 10px * s 同心圆角云母浅灰细外框 (#CBD5E1 -> #B5C2D4，确保贴在白色背景上也边界分明)
        const float borderInset = 0.75f * s;
        const float cornerR = (std::max)(2.0f, 10.0f * s - borderInset);
        const D2D1_GRADIENT_STOP rimStops[2] = {
            {0.0f, D2D1::ColorF(0.796f, 0.835f, 0.882f, 0.95f)},
            {1.0f, D2D1::ColorF(0.710f, 0.761f, 0.831f, 0.95f)},
        };
        ComPtr<ID2D1GradientStopCollection> rimCol;
        ComPtr<ID2D1LinearGradientBrush> rimBrush;
        if (SUCCEEDED(rt->CreateGradientStopCollection(rimStops, 2, rimCol.GetAddressOf())) && rimCol) {
            rt->CreateLinearGradientBrush(
                D2D1::LinearGradientBrushProperties(D2D1::Point2F(0.0f, 0.0f), D2D1::Point2F(0.0f, fh)),
                rimCol.Get(), rimBrush.GetAddressOf());
            if (rimBrush) {
                const D2D1_ROUNDED_RECT rimRect = D2D1::RoundedRect(
                    D2D1::RectF(borderInset, borderInset, fw - borderInset, fh - borderInset),
                    cornerR, cornerR);
                rt->DrawRoundedRectangle(rimRect, rimBrush.Get(), 1.2f * s);
            }
        }

        // 4. 高对比度深邃石墨黑正文渲染 (#0F172A)
        ComPtr<ID2D1SolidColorBrush> textBrush;
        rt->CreateSolidColorBrush(D2D1::ColorF(0.059f, 0.090f, 0.165f, 0.98f), textBrush.GetAddressOf());
        if (textBrush) {
            rt->DrawTextLayout(
                D2D1::Point2F(padLeft, padTop),
                textLayout.Get(),
                textBrush.Get(),
                D2D1_DRAW_TEXT_OPTIONS_CLIP);
        }
    }, result);

    return result;
}

namespace {

void inspectClipboardTextAndPriority(std::wstring& outText, bool& outPreferTextOverImage) {
    outText.clear();
    outPreferTextOverImage = false;

    bool opened = false;
    for (int retry = 0; retry < 40; ++retry) {
        if (OpenClipboard(nullptr)) {
            SetLastError(0);
            const UINT probeFmt = EnumClipboardFormats(0);
            if ((probeFmt == 0 && GetLastError() == ERROR_CLIPBOARD_NOT_OPEN) ||
                (probeFmt != 0 && GetClipboardData(probeFmt) == nullptr && GetLastError() == ERROR_CLIPBOARD_NOT_OPEN)) {
                CloseClipboard();
                Sleep(10);
                continue;
            }
            opened = true;
            break;
        }
        Sleep(5);
    }
    if (!opened) return;

    const UINT formatPinMeta = RegisterClipboardFormatW(L"Tools3000_PinMetadata");
    const UINT formatPng = RegisterClipboardFormatW(L"PNG");
    const bool hasPinMeta = (formatPinMeta != 0 && IsClipboardFormatAvailable(formatPinMeta));

    if (!hasPinMeta) {
        for (UINT fmt = EnumClipboardFormats(0); fmt != 0; fmt = EnumClipboardFormats(fmt)) {
            if (fmt == CF_UNICODETEXT || fmt == CF_TEXT || fmt == CF_OEMTEXT) {
                outPreferTextOverImage = true;
                break;
            }
            if ((formatPng != 0 && fmt == formatPng) ||
                fmt == CF_DIBV5 || fmt == CF_DIB || fmt == CF_BITMAP) {
                outPreferTextOverImage = false;
                break;
            }
        }
    }

    if (IsClipboardFormatAvailable(CF_UNICODETEXT)) {
        if (HANDLE h = GetClipboardData(CF_UNICODETEXT)) {
            if (auto* p = static_cast<const wchar_t*>(GlobalLock(h))) {
                outText = p;
                GlobalUnlock(h);
            }
        }
    }
    CloseClipboard();
}

cv::Mat renderClipboardTextOrColor(const std::wstring& clipText, float& outCornerRadius, std::wstring& outCleanSource) {
    outCornerRadius = 0.0f;
    outCleanSource.clear();
    if (clipText.empty()) return {};

    const float scale = queryActiveMonitorScale();
    cv::Scalar c;
    std::wstring hexLabel;
    if (PinPasteCoordinator::parseHexColor(clipText, c, hexLabel)) {
        cv::Mat swatch = PinPasteCoordinator::renderColorSwatch(c, hexLabel, scale);
        if (!swatch.empty()) {
            outCornerRadius = 10.0f * scale;
            outCleanSource = hexLabel;
        }
        return swatch;
    }

    cv::Mat note = PinPasteCoordinator::renderTextToImage(clipText, scale);
    if (!note.empty()) {
        outCornerRadius = 10.0f * scale;
        outCleanSource = trimClipboardSourceText(clipText);
    }
    return note;
}

} // namespace

uint64_t PinPasteCoordinator::computeContentFingerprint(const cv::Mat& image, const std::wstring& sourceText) noexcept {
    constexpr uint64_t kFnvOffset = 14695981039346656037ULL;
    constexpr uint64_t kFnvPrime = 1099511628211ULL;
    uint64_t h = kFnvOffset;
    auto mix64 = [&](uint64_t v) noexcept {
        for (int i = 0; i < 8; ++i) {
            h ^= (v & 0xFFu);
            h *= kFnvPrime;
            v >>= 8;
        }
    };

    if (!sourceText.empty()) {
        mix64(0x54585431ULL); // "TXT1"
        mix64(static_cast<uint64_t>(sourceText.size()));
        for (wchar_t ch : sourceText) {
            h ^= static_cast<uint64_t>(static_cast<uint16_t>(ch) & 0xFFu);
            h *= kFnvPrime;
            h ^= static_cast<uint64_t>((static_cast<uint16_t>(ch) >> 8) & 0xFFu);
            h *= kFnvPrime;
        }
        return h;
    }

    if (image.empty() || image.cols <= 0 || image.rows <= 0) {
        return 0;
    }

    mix64(0x494D4731ULL); // "IMG1"
    mix64(static_cast<uint64_t>(image.cols));
    mix64(static_cast<uint64_t>(image.rows));

    const int ch = image.channels();
    const int elemBytes = (std::min)(ch, 3); // 仅比较 RGB，消除 BGR(24位) 与 BGRA(32位) 透明封装差异误判
    const int gridX = (std::min)(16, image.cols);
    const int gridY = (std::min)(16, image.rows);
    for (int gy = 0; gy < gridY; ++gy) {
        const int y = (gridY == 1) ? 0 : (gy * (image.rows - 1)) / (gridY - 1);
        const uint8_t* rowPtr = image.ptr<uint8_t>(y);
        for (int gx = 0; gx < gridX; ++gx) {
            const int x = (gridX == 1) ? 0 : (gx * (image.cols - 1)) / (gridX - 1);
            const uint8_t* px = rowPtr + static_cast<size_t>(x) * static_cast<size_t>(ch);
            for (int c = 0; c < elemBytes; ++c) {
                h ^= static_cast<uint64_t>(px[c]);
                h *= kFnvPrime;
            }
        }
    }
    return h;
}

PinPasteCoordinator& PinPasteCoordinator::instance() {
    static PinPasteCoordinator coord;
    return coord;
}

void PinPasteCoordinator::reset() {
    std::lock_guard<std::mutex> lock(m_mutex);
    m_reverseIndex = 0;
    m_lastTriggerTime = {};
    m_lastClipboardSeq = 0;
    m_lastHistoryCount = 0;
    m_lastHistoryHeadId = 0;
}

int PinPasteCoordinator::currentReverseIndex() const {
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_reverseIndex;
}

bool PinPasteCoordinator::isSessionActive() const {
    std::lock_guard<std::mutex> lock(m_mutex);
    if (m_lastTriggerTime.time_since_epoch().count() <= 0) return false;
    auto now = std::chrono::steady_clock::now();
    auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(now - m_lastTriggerTime).count();
    return elapsed <= m_sessionTimeoutMs;
}

void PinPasteCoordinator::setSessionTimeoutMs(int timeoutMs) {
    std::lock_guard<std::mutex> lock(m_mutex);
    m_sessionTimeoutMs = (std::max)(500, timeoutMs);
}

std::vector<PinPasteCandidate> PinPasteCoordinator::resolveCandidates([[maybe_unused]] DWORD currentSeq, int historyCount) {
    std::vector<PinPasteCandidate> candidates;

    // 1. 探测系统剪贴板中的文本与图像优先级（当富文本编辑器/Office 将 CF_UNICODETEXT 置于首位且附带预览位图时，优先转为微晶字签卡片）
    std::wstring clipText;
    bool preferTextOverImage = false;
    inspectClipboardTextAndPriority(clipText, preferTextOverImage);

    Tools3000PinMetadata meta{};
    meta.magic = 0;
    cv::Mat clipImg;
    bool clipFromTextOrColor = false;
    float generatedCornerRadius = 0.0f;
    std::wstring generatedSourceText;

    if (preferTextOverImage && !clipText.empty()) {
        clipImg = renderClipboardTextOrColor(clipText, generatedCornerRadius, generatedSourceText);
        clipFromTextOrColor = !clipImg.empty();
    }

    if (clipImg.empty()) {
        clipImg = ClipboardUtils::readImageFromClipboard(&meta);
    }

    if (clipImg.empty() && !clipText.empty()) {
        clipImg = renderClipboardTextOrColor(clipText, generatedCornerRadius, generatedSourceText);
        clipFromTextOrColor = !clipImg.empty();
    }

    // 2. 检查 CaptureHistory 中的条目
    auto& history = CaptureHistory::instance();
    const int count = (historyCount >= 0) ? historyCount : history.count();
    const uint64_t clipFp = !clipImg.empty()
        ? computeContentFingerprint(clipImg, clipFromTextOrColor ? generatedSourceText : std::wstring{})
        : 0ULL;

    // 检查剪贴板图像是否与 CaptureHistory[0] 像素内容完全一致（用 64-bit 像素指纹精准判重，
    // 绝不因两张不同截图恰好同尺寸而误吞新图；微晶字签/色卡绝不与历史截图判重）
    bool clipboardMatchesHistory0 = false;
    if (!clipImg.empty() && !clipFromTextOrColor && count > 0) {
        auto latestOpt = history.get(0);
        if (latestOpt.has_value() && !latestOpt->image.empty()) {
            const auto& latest = *latestOpt;
            const uint64_t latestFp = computeContentFingerprint(latest.image);
            if (clipFp != 0ULL && latestFp != 0ULL && clipFp == latestFp) {
                clipboardMatchesHistory0 = true;
            }
        }
    }

    // 3. 构造候选列表
    if (!clipImg.empty() && !clipboardMatchesHistory0) {
        PinPasteCandidate clipCand;
        clipCand.image = clipImg;
        if (!clipFromTextOrColor && meta.magic == 0x54333030 && meta.regionW > 0 && meta.regionH > 0) {
            clipCand.region = CaptureRegion{meta.regionX, meta.regionY, meta.regionW, meta.regionH, meta.cornerRadius};
            clipCand.padX = meta.padX;
            clipCand.padY = meta.padY;
        }
        clipCand.isFromClipboard = true;
        clipCand.isGeneratedCard = clipFromTextOrColor;
        clipCand.cardCornerRadius = generatedCornerRadius;
        clipCand.sourceText = generatedSourceText;
        clipCand.contentFingerprint = clipFp;
        candidates.push_back(std::move(clipCand));
    }

    // 依序推入 CaptureHistory 中的所有截图（从 0 到 count-1，即按保存时间倒序排列）
    for (int i = 0; i < count; ++i) {
        auto entryOpt = history.get(i);
        if (!entryOpt.has_value() || entryOpt->image.empty()) continue;
        const auto& entry = *entryOpt;
        PinPasteCandidate histCand;
        histCand.image = entry.image;
        histCand.region = entry.region;
        histCand.timestamp = entry.timestamp;
        if (entry.region.width > 0 && entry.image.cols > entry.region.width) {
            histCand.padX = (entry.image.cols - entry.region.width) / 2;
        }
        if (entry.region.height > 0 && entry.image.rows > entry.region.height) {
            histCand.padY = (entry.image.rows - entry.region.height) / 2;
        }
        histCand.isFromClipboard = false;
        histCand.contentFingerprint = computeContentFingerprint(entry.image);
        candidates.push_back(std::move(histCand));
    }

    if (candidates.empty() && !clipImg.empty()) {
        PinPasteCandidate clipCand;
        clipCand.image = clipImg;
        clipCand.isFromClipboard = true;
        clipCand.isGeneratedCard = clipFromTextOrColor;
        clipCand.cardCornerRadius = generatedCornerRadius;
        clipCand.sourceText = generatedSourceText;
        clipCand.contentFingerprint = clipFp;
        candidates.push_back(std::move(clipCand));
    }

    return candidates;
}

std::optional<PinPasteDecision> PinPasteCoordinator::stepNext(
    const POINT* fallbackCursor,
    const std::unordered_set<uint64_t>* activeDesktopFingerprints) {
    std::lock_guard<std::mutex> lock(m_mutex);

    auto now = std::chrono::steady_clock::now();
    DWORD currentSeq = GetClipboardSequenceNumber();
    auto& history = CaptureHistory::instance();
    int historyCount = history.count();
    uint64_t historyHeadId = 0;
    if (historyCount > 0) {
        if (auto headOpt = history.get(0); headOpt.has_value()) {
            historyHeadId = headOpt->id;
        }
    }

    bool sessionValid = false;
    if (m_lastTriggerTime.time_since_epoch().count() > 0) {
        auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(now - m_lastTriggerTime).count();
        if (elapsed <= m_sessionTimeoutMs &&
            currentSeq == m_lastClipboardSeq &&
            historyCount == m_lastHistoryCount &&
            historyHeadId == m_lastHistoryHeadId) {
            sessionValid = true;
        }
    }

    if (!sessionValid) {
        m_reverseIndex = 0;
    } else {
        m_reverseIndex++;
    }

    auto candidates = resolveCandidates(currentSeq, historyCount);
    if (candidates.empty()) {
        LOG_INFO("PinPasteCoordinator: 剪贴板与历史记录中无可贴内容");
        m_reverseIndex = 0;
        return std::nullopt;
    }

    const int totalCandidates = static_cast<int>(candidates.size());
    int targetIndex = m_reverseIndex % totalCandidates;
    bool alreadyVisibleOnDesktop = false;

    // 真实桌面防重复套娃机制：当传入当前虚拟桌面已显示的贴图指纹集合时，
    // 智能跳过已贴在桌面上的重复条目；若全部候选均已在桌面显示则禁止无限套娃克隆，转由调用方置顶聚焦并提示
    if (activeDesktopFingerprints && !activeDesktopFingerprints->empty()) {
        auto isAlreadyVisible = [&](int idx) -> bool {
            const uint64_t fp = candidates[static_cast<size_t>(idx)].contentFingerprint;
            return fp != 0ULL && activeDesktopFingerprints->find(fp) != activeDesktopFingerprints->end();
        };

        int chosen = -1;
        for (int offset = 0; offset < totalCandidates; ++offset) {
            const int probe = (targetIndex + offset) % totalCandidates;
            if (!isAlreadyVisible(probe)) {
                chosen = probe;
                break;
            }
        }
        if (chosen >= 0) {
            targetIndex = chosen;
            m_reverseIndex = chosen;
        } else {
            alreadyVisibleOnDesktop = true;
            targetIndex = 0;
            m_reverseIndex = 0;
        }
    }

    const auto& candidate = candidates[static_cast<size_t>(targetIndex)];

    POINT cursor{};
    if (fallbackCursor) {
        cursor = *fallbackCursor;
    } else {
        GetCursorPos(&cursor);
    }

    HMONITOR mon = MonitorFromPoint(cursor, MONITOR_DEFAULTTONEAREST);
    RECT work = tools3000::core::dpi::workArea(mon);
    const float monScale = tools3000::core::dpi::scaleForMonitor(mon);

    int minVx = GetSystemMetrics(SM_XVIRTUALSCREEN);
    int minVy = GetSystemMetrics(SM_YVIRTUALSCREEN);
    int maxVw = GetSystemMetrics(SM_CXVIRTUALSCREEN);
    int maxVh = GetSystemMetrics(SM_CYVIRTUALSCREEN);
    RECT vScreen{minVx, minVy, minVx + maxVw, minVy + maxVh};

    PinSpawnInput input;
    input.imageWidth = candidate.image.cols;
    input.imageHeight = candidate.image.rows;
    input.padX = candidate.padX;
    input.padY = candidate.padY;
    input.cursor = cursor;
    input.workArea = work;
    input.virtualScreen = vScreen;
    input.stackVerticallyOnIntersect = candidate.isGeneratedCard;
    input.stackGap = tools3000::core::dpi::scaleMetric(8, monScale);

    if (!candidate.isGeneratedCard && candidate.region.width > 0 && candidate.region.height > 0) {
        input.preferredRegion = PinSpawnRegion{
            candidate.region.x, candidate.region.y,
            candidate.region.width, candidate.region.height
        };
    }

    // 动态探测当前虚拟桌面所有现有活跃贴图窗口，用于阶梯避让或微晶便签栈垂直排布（隔离其他虚拟桌面窗口）
    HWND hExisting = nullptr;
    while ((hExisting = FindWindowExW(nullptr, hExisting, L"Tools3000_PinWindow", nullptr)) != nullptr) {
        if (tools3000::core::WinUtils::isWindowVisibleOnCurrentDesktop(hExisting)) {
            RECT rc{};
            GetWindowRect(hExisting, &rc);
            input.existingPins.push_back(rc);
        }
    }

    POINT spawnPos = PinSpawnCalculator::calculate(input);

    PinPasteDecision decision;
    decision.candidate = candidate;
    decision.spawnPos = spawnPos;
    decision.reverseIndex = m_reverseIndex;
    decision.targetIndex = targetIndex;
    decision.totalCount = totalCandidates;
    decision.isLooped = (m_reverseIndex > 0 && targetIndex == 0);
    decision.alreadyVisibleOnDesktop = alreadyVisibleOnDesktop;

    m_lastTriggerTime = now;
    m_lastClipboardSeq = currentSeq;
    m_lastHistoryCount = historyCount;
    m_lastHistoryHeadId = historyHeadId;

    LOG_INFO("PinPasteCoordinator: 贴出历史条目 [{}/{}], 尺寸={}x{}, 坐标=({}, {})",
             targetIndex + 1, totalCandidates, candidate.image.cols, candidate.image.rows, spawnPos.x, spawnPos.y);

    return decision;
}

} // namespace tools3000::capture
