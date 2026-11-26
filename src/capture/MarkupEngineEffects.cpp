// ─────────────────────────────────────────────────────────────────────────────
// MarkupEngineEffects.cpp — 马赛克/模糊/文本/序号/放大镜/聚光灯/水印/消除/原位改字处理器子模块
// ─────────────────────────────────────────────────────────────────────────────

#ifndef NOMINMAX
#define NOMINMAX
#endif

#include "capture/MarkupEngineInternal.h"
#include "core/i18n/UiI18n.h"
#include "capture/TextAnnotationHelper.h"
#include "capture/TextReplaceEngine.h"
#include "capture/MarkupCurveGeometry.h"
#include "capture/CaptureInteractionPolicy.h"
#include "capture/CaptureTextEditor.h"
#include "capture/CornerRadiusHelper.h"
#include "core/logger/Logger.h"
#include "core/utils/DWriteFontUtils.h"
#include "core/utils/TraceId.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <limits>
#include <mutex>
#include <windows.h>
#include <objidl.h>
#include <gdiplus.h>
#pragma comment(lib, "gdiplus.lib")
#include <opencv2/imgproc.hpp>
#include <opencv2/photo.hpp>

namespace tools3000::capture {

using namespace markup_detail;

namespace {
    std::wstring utf8ToWide(const std::string& str) {
        if (str.empty()) return L"";
        int len = MultiByteToWideChar(CP_UTF8, 0, str.c_str(), -1, nullptr, 0);
        if (len <= 0) return L"";
        std::wstring wstr(len - 1, 0);
        MultiByteToWideChar(CP_UTF8, 0, str.c_str(), -1, &wstr[0], len);
        return wstr;
    }

    void blendOverlay(cv::Mat& canvas, const cv::Mat& fgMat, int startX, int startY) {
        if (canvas.empty() || fgMat.empty()) return;
        int szH = fgMat.rows;
        int szW = fgMat.cols;

        if (canvas.channels() == 3) {
            for (int y = 0; y < szH; ++y) {
                int targetY = startY + y;
                if (targetY < 0 || targetY >= canvas.rows) continue;
                cv::Vec3b* rowBg = canvas.ptr<cv::Vec3b>(targetY);
                const cv::Vec4b* rowFg = fgMat.ptr<cv::Vec4b>(y);
                for (int x = 0; x < szW; ++x) {
                    int targetX = startX + x;
                    if (targetX < 0 || targetX >= canvas.cols) continue;

                    const cv::Vec4b& fg = rowFg[x];
                    float alpha = fg[3] / 255.0f;
                    if (alpha > 0.001f) {
                        cv::Vec3b& bg = rowBg[targetX];
                        bg[0] = static_cast<uchar>(std::clamp(static_cast<int>(std::lround(fg[0] * alpha + bg[0] * (1.0f - alpha))), 0, 255));
                        bg[1] = static_cast<uchar>(std::clamp(static_cast<int>(std::lround(fg[1] * alpha + bg[1] * (1.0f - alpha))), 0, 255));
                        bg[2] = static_cast<uchar>(std::clamp(static_cast<int>(std::lround(fg[2] * alpha + bg[2] * (1.0f - alpha))), 0, 255));
                    }
                }
            }
        } else if (canvas.channels() == 4) {
            for (int y = 0; y < szH; ++y) {
                int targetY = startY + y;
                if (targetY < 0 || targetY >= canvas.rows) continue;
                cv::Vec4b* rowBg = canvas.ptr<cv::Vec4b>(targetY);
                const cv::Vec4b* rowFg = fgMat.ptr<cv::Vec4b>(y);
                for (int x = 0; x < szW; ++x) {
                    int targetX = startX + x;
                    if (targetX < 0 || targetX >= canvas.cols) continue;

                    const cv::Vec4b& fg = rowFg[x];
                    float alpha = fg[3] / 255.0f;
                    if (alpha > 0.001f) {
                        cv::Vec4b& bg = rowBg[targetX];
                        bg[0] = static_cast<uchar>(std::clamp(static_cast<int>(std::lround(fg[0] * alpha + bg[0] * (1.0f - alpha))), 0, 255));
                        bg[1] = static_cast<uchar>(std::clamp(static_cast<int>(std::lround(fg[1] * alpha + bg[1] * (1.0f - alpha))), 0, 255));
                        bg[2] = static_cast<uchar>(std::clamp(static_cast<int>(std::lround(fg[2] * alpha + bg[2] * (1.0f - alpha))), 0, 255));
                        bg[3] = 255;
                    }
                }
            }
        }
    }
    cv::Size measureSnipasteText(const std::wstring& text, float fontSize, bool /*isEditing*/ = false,
                                 float maxBoxWidth = 0.0f,
                                 bool fixedBoxWidth = false,
                                 float minBoxHeight = 0.0f) {
        ensureGdiplusStartup();
        const float safeFontSize = (std::max)(12.0f, fontSize);
        const int padW = tools3000::capture::CaptureTextEditor::computeBoxHorizontalPadding(safeFontSize);
        const int padH = tools3000::capture::CaptureTextEditor::computeBoxVerticalPadding(safeFontSize);
        if (!isGdiplusStarted()) {
            int w = text.empty() ? (124 + padW) : static_cast<int>((std::max)(40.0f, safeFontSize * 2.0f)) + padW;
            if (fixedBoxWidth && maxBoxWidth > 0.0f) {
                w = (std::max)(w, static_cast<int>(std::ceil(maxBoxWidth)));
            }
            int h = static_cast<int>((std::max)(24.0f, safeFontSize * 1.3f)) + padH;
            if (minBoxHeight > 0.0f) {
                h = (std::max)(h, static_cast<int>(std::ceil(minBoxHeight)));
            }
            return cv::Size((std::max)(w, 24 + padW), (std::max)(h, static_cast<int>(std::ceil(safeFontSize)) + padH));
        }

        const std::wstring placeholder = tools3000::core::trWCstr(L"点击输入文字...", L"Click to enter text...");
        const std::wstring& measureStr = text.empty() ? placeholder : text;
        const float maxContentW = tools3000::capture::CaptureTextEditor::computeContentWidthFromBoxWidth(
            maxBoxWidth, safeFontSize);
        const auto layout = tools3000::capture::CaptureTextEditor::buildLayout(
            measureStr, safeFontSize, maxContentW);
        const auto [w, h] = tools3000::capture::CaptureTextEditor::computeBoxSize(
            layout, safeFontSize, maxBoxWidth, fixedBoxWidth, minBoxHeight);
        return cv::Size(w, h);
    }

    void renderSnipasteStyleText(cv::Mat& canvas, const std::string& text, cv::Point pt, const cv::Scalar& color,
                                 float fontSize, bool isEditing, bool isSelected, bool withBackdrop, cv::Size& outSize,
                                 bool textOutline = true,
                                 const tools3000::capture::MarkupColor& outlineColor = tools3000::capture::MarkupColor::Auto(),
                                 size_t caretIndex = 0, size_t selAnchor = 0, uint32_t lastCaretTick = 0,
                                 float outlineThicknessScale = 1.0f,
                                 float maxBoxWidth = 0.0f,
                                 bool fixedBoxWidth = false,
                                 float minBoxHeight = 0.0f) {
        ensureGdiplusStartup();
        std::wstring wtext = utf8ToWide(text);
        if (wtext.empty() && !isEditing && !isSelected) {
            outSize = cv::Size(0, 0);
            return;
        }

        const float safeFontSize = (std::max)(12.0f, fontSize);
        const float maxContentW = tools3000::capture::CaptureTextEditor::computeContentWidthFromBoxWidth(
            maxBoxWidth, safeFontSize);
        const auto textLayout = tools3000::capture::CaptureTextEditor::buildLayout(
            wtext, safeFontSize, maxContentW);

        cv::Size sz(0, 0);
        if (wtext.empty()) {
            sz = measureSnipasteText(wtext, safeFontSize, isEditing || isSelected,
                                     maxBoxWidth, fixedBoxWidth, minBoxHeight);
        } else {
            const auto [bw, bh] = tools3000::capture::CaptureTextEditor::computeBoxSize(
                textLayout, safeFontSize, maxBoxWidth, fixedBoxWidth, minBoxHeight);
            sz = cv::Size(bw, bh);
        }
        outSize = sz;
        if (sz.width <= 0 || sz.height <= 0) return;

        cv::Mat textMat(sz, CV_8UC4, cv::Scalar(0, 0, 0, 0));
        {
            Gdiplus::Bitmap bitmap(sz.width, sz.height, static_cast<INT>(textMat.step), PixelFormat32bppARGB, textMat.data);
            Gdiplus::Graphics g(&bitmap);
            g.SetSmoothingMode(Gdiplus::SmoothingModeHighQuality);
            g.SetPixelOffsetMode(Gdiplus::PixelOffsetModeHighQuality);
            // 采用纯净灰度抗锯齿 (TextRenderingHintAntiAliasGridFit)，严禁在透明图层上使用 ClearType 造成彩色/黑白栅格条纹
            g.SetTextRenderingHint(Gdiplus::TextRenderingHintAntiAliasGridFit);

            BYTE rVal = static_cast<BYTE>(color[2]);
            BYTE gVal = static_cast<BYTE>(color[1]);
            BYTE bVal = static_cast<BYTE>(color[0]);
            float luminance = (0.299f * rVal + 0.587f * gVal + 0.114f * bVal) / 255.0f;

            // 1. 文字绘制 (纯透明底衬 + 选区高亮 + 高对比描边 + 矢量文字填充)
            Gdiplus::FontFamily fontFamily(tools3000::capture::CaptureTextEditor::resolveMarkupFontFamilyName(false));
            const Gdiplus::FontFamily* activeFamily = &fontFamily;
            Gdiplus::FontFamily fallbackFamily(L"Segoe UI");
            if (!fontFamily.IsAvailable()) {
                activeFamily = &fallbackFamily;
            }

            Gdiplus::StringFormat format(Gdiplus::StringFormat::GenericTypographic());
            format.SetAlignment(Gdiplus::StringAlignmentNear);
            format.SetLineAlignment(Gdiplus::StringAlignmentNear);
            format.SetFormatFlags(format.GetFormatFlags() | Gdiplus::StringFormatFlagsMeasureTrailingSpaces);

            const float textX = textLayout.textOriginX;
            const float textY = textLayout.textOriginY;

            Gdiplus::GraphicsPath boxPath;
            const float boxInset = 2.2f;
            const float bx = boxInset;
            const float by = boxInset;
            const float bw = (std::max)(8.0f, static_cast<float>(sz.width) - boxInset * 2.0f);
            const float bh = (std::max)(8.0f, static_cast<float>(sz.height) - boxInset * 2.0f);
            const float r = (std::min)({6.0f, bw * 0.5f, bh * 0.5f});
            boxPath.AddArc(bx, by, r * 2.0f, r * 2.0f, 180.0f, 90.0f);
            boxPath.AddArc(bx + bw - r * 2.0f, by, r * 2.0f, r * 2.0f, 270.0f, 90.0f);
            boxPath.AddArc(bx + bw - r * 2.0f, by + bh - r * 2.0f, r * 2.0f, r * 2.0f, 0.0f, 90.0f);
            boxPath.AddArc(bx, by + bh - r * 2.0f, r * 2.0f, r * 2.0f, 90.0f, 90.0f);
            boxPath.CloseFigure();

            if (withBackdrop) {
                // 仅当用户显式开启填充底衬开关时，绘制现代圆角微晶背景胶囊与玻璃描边
                Gdiplus::SolidBrush bgBrush(luminance > 0.45f ? Gdiplus::Color(190, 15, 23, 42) : Gdiplus::Color(200, 245, 247, 250));
                g.FillPath(&bgBrush, &boxPath);
                Gdiplus::Pen glassBorder(luminance > 0.45f ? Gdiplus::Color(60, 255, 255, 255) : Gdiplus::Color(40, 0, 0, 0), 1.0f);
                g.DrawPath(&glassBorder, &boxPath);
            }

            if (isEditing) {
                // 编辑态仅绘制半透明亮蓝虚线框指示输入焦点，背景 100% 保持透明透出原图，绝不遮挡原图
                Gdiplus::Pen editHaloPen(Gdiplus::Color(90, 15, 23, 42), 2.4f);
                g.DrawPath(&editHaloPen, &boxPath);
                Gdiplus::Pen editFocusPen(Gdiplus::Color(225, 59, 130, 246), 1.5f);
                editFocusPen.SetDashStyle(Gdiplus::DashStyleDash);
                g.DrawPath(&editFocusPen, &boxPath);

                // 绘制多行文本选区高亮微晶底块（支持鼠标拖拽划词、自动软换行跨行选区与 Ctrl+A 全选）
                if (tools3000::capture::CaptureTextEditor::hasSelection(caretIndex, selAnchor) && !wtext.empty()) {
                    const auto spans = tools3000::capture::CaptureTextEditor::computeSelectionHighlights(
                        textLayout, wtext, caretIndex, selAnchor);
                    Gdiplus::SolidBrush selFill(Gdiplus::Color(135, 59, 130, 246));
                    Gdiplus::Pen selBorder(Gdiplus::Color(190, 96, 165, 250), 1.0f);
                    for (const auto& sp : spans) {
                        const float sr = std::min({3.5f, sp.width * 0.5f, sp.height * 0.5f});
                        Gdiplus::GraphicsPath spPath;
                        if (sr > 1.0f) {
                            const float d = sr * 2.0f;
                            spPath.AddArc(sp.x, sp.y, d, d, 180.0f, 90.0f);
                            spPath.AddArc(sp.x + sp.width - d, sp.y, d, d, 270.0f, 90.0f);
                            spPath.AddArc(sp.x + sp.width - d, sp.y + sp.height - d, d, d, 0.0f, 90.0f);
                            spPath.AddArc(sp.x, sp.y + sp.height - d, d, d, 90.0f, 90.0f);
                            spPath.CloseFigure();
                        } else {
                            spPath.AddRectangle(Gdiplus::RectF(sp.x, sp.y, sp.width, sp.height));
                        }
                        g.FillPath(&selFill, &spPath);
                        g.DrawPath(&selBorder, &spPath);
                    }
                }
            }

            if (!wtext.empty()) {
                // 关键铁律：必须显式使用 FillModeWinding（非零环绕规则）！
                // 思源黑体 (Noto Sans SC / Source Han Sans SC) 等现代可变/OpenType 中文字库采用独立笔画闭合轮廓交叠构字，
                // 默认的 FillModeAlternate（奇偶规则）会在横竖撇捺所有笔画交叉处产生偶次挖空孔洞，导致底层白色描边穿透形成断裂虚线杂色。
                // 逐视觉行按 SSOT CaptureTextEditor::buildLayout 的精确 topY 写入同一路径，保证编辑预览、光标、选区与导出位图 100% 像素级同构。
                Gdiplus::GraphicsPath textPath(Gdiplus::FillModeWinding);
                for (const auto& line : textLayout.lines) {
                    if (line.endIndex <= line.startIndex) continue;
                    const std::wstring lineSlice = wtext.substr(line.startIndex, line.endIndex - line.startIndex);
                    textPath.AddString(
                        lineSlice.c_str(),
                        static_cast<INT>(lineSlice.size()),
                        activeFamily,
                        Gdiplus::FontStyleBold,
                        static_cast<Gdiplus::REAL>(safeFontSize),
                        Gdiplus::PointF(textX, line.topY),
                        &format);
                }

                const BYTE elemAlpha = (color[3] > 0) ? static_cast<BYTE>(std::clamp(color[3], 0.0, 255.0)) : 255;
                auto drawGlyphAndOutline = [&](Gdiplus::Graphics& targetG, BYTE masterAlpha) {
                    if (textOutline) {
                        const tools3000::capture::MarkupColor textColor{rVal, gVal, bVal, 255};
                        const tools3000::capture::MarkupColor resolvedOutline =
                            tools3000::capture::resolveTextOutlineColor(
                                textColor, outlineColor, canvas,
                                cv::Rect(pt.x, pt.y, sz.width, sz.height), withBackdrop);
                        const BYTE baseOutA = resolvedOutline.a > 0 ? resolvedOutline.a : 250;
                        const BYTE outA = (masterAlpha == 255)
                            ? baseOutA
                            : static_cast<BYTE>(std::lround(static_cast<float>(baseOutA) * (static_cast<float>(masterAlpha) / 255.0f)));
                        Gdiplus::Color strokeColor(outA, resolvedOutline.r, resolvedOutline.g, resolvedOutline.b);
                        const float strokeWidth = tools3000::capture::CaptureTextEditor::computeOutlineStrokeWidth(safeFontSize) *
                                                  std::clamp(outlineThicknessScale, 0.25f, 4.0f);
                        Gdiplus::Pen strokePen(strokeColor, strokeWidth);
                        strokePen.SetLineJoin(Gdiplus::LineJoinRound);
                        targetG.DrawPath(&strokePen, &textPath);
                    }

                    // 正文矢量文字以 255 满不透明度配合 FillModeWinding 严密覆盖一切内部交叠笔画描边
                    Gdiplus::SolidBrush textBrush(Gdiplus::Color(masterAlpha, rVal, gVal, bVal));
                    targetG.FillPath(&textBrush, &textPath);
                };

                if (textOutline && elemAlpha < 255) {
                    // 半透明带描边文字：先在独立透明层完成 100% 实心遮罩合成（消除内部重叠笔画内描边透出），再按整体透明度缩放叠入
                    cv::Mat glyphLayer(sz, CV_8UC4, cv::Scalar(0, 0, 0, 0));
                    {
                        Gdiplus::Bitmap glyphBmp(sz.width, sz.height, static_cast<INT>(glyphLayer.step), PixelFormat32bppARGB, glyphLayer.data);
                        Gdiplus::Graphics gg(&glyphBmp);
                        gg.SetSmoothingMode(Gdiplus::SmoothingModeHighQuality);
                        gg.SetPixelOffsetMode(Gdiplus::PixelOffsetModeHighQuality);
                        gg.SetTextRenderingHint(Gdiplus::TextRenderingHintAntiAliasGridFit);
                        drawGlyphAndOutline(gg, 255);
                    }
                    const float opScale = static_cast<float>(elemAlpha) / 255.0f;
                    for (int y = 0; y < sz.height; ++y) {
                        cv::Vec4b* dstRow = textMat.ptr<cv::Vec4b>(y);
                        const cv::Vec4b* srcRow = glyphLayer.ptr<cv::Vec4b>(y);
                        for (int x = 0; x < sz.width; ++x) {
                            const float sa = (srcRow[x][3] / 255.0f) * opScale;
                            if (sa <= 0.001f) continue;
                            const float da = dstRow[x][3] / 255.0f;
                            const float outA = sa + da * (1.0f - sa);
                            if (outA > 1e-4f) {
                                dstRow[x][0] = static_cast<uchar>(std::clamp(static_cast<int>(std::lround((srcRow[x][0] * sa + dstRow[x][0] * da * (1.0f - sa)) / outA)), 0, 255));
                                dstRow[x][1] = static_cast<uchar>(std::clamp(static_cast<int>(std::lround((srcRow[x][1] * sa + dstRow[x][1] * da * (1.0f - sa)) / outA)), 0, 255));
                                dstRow[x][2] = static_cast<uchar>(std::clamp(static_cast<int>(std::lround((srcRow[x][2] * sa + dstRow[x][2] * da * (1.0f - sa)) / outA)), 0, 255));
                                dstRow[x][3] = static_cast<uchar>(std::clamp(static_cast<int>(std::lround(outA * 255.0f)), 0, 255));
                            }
                        }
                    }
                } else {
                    drawGlyphAndOutline(g, elemAlpha);
                }
            } else if (isEditing || isSelected) {
                // 空文本编辑态：绘制提示占位文字（严格遵循字重 >= 500 Bold 与字号 >= 12px 排版红线，窄选区同样自动换行不截断）
                const std::wstring placeholder = tools3000::core::trWCstr(L"点击输入文字...", L"Click to enter text...");
                const float phFontSize = (std::max)(12.0f, safeFontSize * 0.88f);
                Gdiplus::Font placeholderFont(activeFamily, static_cast<Gdiplus::REAL>(phFontSize), Gdiplus::FontStyleBold, Gdiplus::UnitPixel);
                Gdiplus::SolidBrush placeholderBrush(Gdiplus::Color(180, 148, 163, 184));
                const float phMaxContentW = tools3000::capture::CaptureTextEditor::computeContentWidthFromBoxWidth(
                    maxBoxWidth, safeFontSize);
                const auto phLayout = tools3000::capture::CaptureTextEditor::buildLayout(
                    placeholder, phFontSize, phMaxContentW);
                for (const auto& line : phLayout.lines) {
                    if (line.endIndex <= line.startIndex) continue;
                    const std::wstring lineSlice = placeholder.substr(line.startIndex, line.endIndex - line.startIndex);
                    g.DrawString(lineSlice.c_str(), static_cast<INT>(lineSlice.size()), &placeholderFont,
                                 Gdiplus::PointF(textX + 2.0f, textY + 1.0f + (line.topY - phLayout.textOriginY)),
                                 &format, &placeholderBrush);
                }
            }

            // 2. 编辑态光标绘制（支持任意视觉行/列精确定位，且在移动光标或输入后的 520ms 内保持常亮）
            if (isEditing) {
                const uint32_t nowTick = GetTickCount();
                const bool recentlyMoved = (lastCaretTick != 0) && ((nowTick - lastCaretTick) < 520u);
                const bool showCursor = recentlyMoved || ((nowTick / 480u) % 2u == 0u) || wtext.empty();
                if (showCursor) {
                    const auto caretVis = tools3000::capture::CaptureTextEditor::computeCaretVisual(
                        textLayout, wtext, caretIndex);
                    Gdiplus::Pen caretShadowPen(Gdiplus::Color(180, 255, 255, 255), 3.2f);
                    caretShadowPen.SetStartCap(Gdiplus::LineCapRound);
                    caretShadowPen.SetEndCap(Gdiplus::LineCapRound);
                    Gdiplus::Pen cursorPen(Gdiplus::Color(255, 37, 99, 235), 2.0f);
                    cursorPen.SetStartCap(Gdiplus::LineCapRound);
                    cursorPen.SetEndCap(Gdiplus::LineCapRound);
                    g.DrawLine(&caretShadowPen, caretVis.x, caretVis.y, caretVis.x, caretVis.y + caretVis.height);
                    g.DrawLine(&cursorPen, caretVis.x, caretVis.y, caretVis.x, caretVis.y + caretVis.height);
                }
            }
        }

        // 4. 高性能自适应 Alpha 混合到 canvas (精准处理 3 通道 BGR 与 4 通道 BGRA，杜绝跨步错位栅格)
        blendOverlay(canvas, textMat, pt.x, pt.y);
    }

    void renderSnipasteStyleNumberBadge(cv::Mat& canvas,
                                        int number,
                                        cv::Point center,
                                        const cv::Scalar& color,
                                        bool fill = true,
                                        float dpiScale = 1.0f,
                                        tools3000::capture::NumberBadgeShape shape = tools3000::capture::NumberBadgeShape::Circle,
                                        tools3000::capture::NumberFormat format = tools3000::capture::NumberFormat::Arabic) {
        ensureGdiplusStartup();
        const float scale = dpiScale > 0.0f ? dpiScale : 1.0f;
        const int radius = static_cast<int>(std::round(15.0f * scale));
        const std::wstring numWStr = tools3000::capture::formatNumberBadgeLabel(number, format);

        const bool preferCjkFont = (format == tools3000::capture::NumberFormat::Chinese);
        Gdiplus::FontFamily primaryFamily(tools3000::capture::CaptureTextEditor::resolveMarkupFontFamilyName(!preferCjkFont));
        Gdiplus::FontFamily fallbackFamily(preferCjkFont ? L"Segoe UI" : L"Microsoft YaHei UI");
        const Gdiplus::FontFamily* activeFamily = primaryFamily.IsAvailable() ? &primaryFamily : &fallbackFamily;

        const std::size_t charLen = numWStr.size();
        float emSize = 16.0f * scale;
        if (preferCjkFont) {
            emSize = ((charLen >= 3) ? 12.0f : (charLen == 2 ? 13.2f : 15.0f)) * scale;
        } else {
            emSize = ((charLen >= 4) ? 12.0f : (charLen == 3 ? 12.5f : (charLen == 2 ? 13.8f : 16.0f))) * scale;
        }

        Gdiplus::GraphicsPath textPath(Gdiplus::FillModeWinding);
        Gdiplus::StringFormat genericFormat(Gdiplus::StringFormat::GenericTypographic());
        textPath.AddString(numWStr.c_str(), -1, activeFamily, Gdiplus::FontStyleBold,
                           emSize, Gdiplus::PointF(0.0f, 0.0f), &genericFormat);
        textPath.Flatten(nullptr, 0.05f);
        Gdiplus::RectF pathBounds;
        textPath.GetBounds(&pathBounds);

        float boxH = radius * 2.0f;
        float boxW = boxH;
        if (pathBounds.Width > 0.0f) {
            if (shape == tools3000::capture::NumberBadgeShape::RoundedSquare) {
                // 圆角矩形模式下，多字符（如 VIII、十一、二十五）自动横向延展为精致微晶胶囊，绝不挤字
                boxW = (std::max)(boxH, std::ceil(pathBounds.Width + 14.0f * scale));
            } else if (charLen >= 4 || pathBounds.Width > boxH - 6.0f * scale) {
                // 圆形徽章遇到 4 位及以上超宽序号（如 1000）自动升级为跑道形椭圆胶囊，保障 >= 12px 锐利不溢出
                boxW = (std::max)(boxH, std::ceil(pathBounds.Width + 12.0f * scale));
            }
        }

        const int pad = static_cast<int>(std::round(10.0f * scale));
        int szW = static_cast<int>(std::ceil(boxW)) + pad;
        int szH = static_cast<int>(std::ceil(boxH)) + pad;
        if (szW % 2 != 0) ++szW;
        if (szH % 2 != 0) ++szH;

        cv::Mat badgeMat(szH, szW, CV_8UC4, cv::Scalar(0, 0, 0, 0));
        {
            Gdiplus::Bitmap bitmap(szW, szH, static_cast<INT>(badgeMat.step), PixelFormat32bppARGB, badgeMat.data);
            Gdiplus::Graphics g(&bitmap);
            g.SetSmoothingMode(Gdiplus::SmoothingModeHighQuality);
            g.SetPixelOffsetMode(Gdiplus::PixelOffsetModeHighQuality);
            g.SetTextRenderingHint(Gdiplus::TextRenderingHintAntiAlias);

            const float cx = static_cast<float>(szW / 2) + 0.5f;
            const float cy = static_cast<float>(szH / 2) + 0.5f;

            const BYTE rVal = static_cast<BYTE>(color[2]);
            const BYTE gVal = static_cast<BYTE>(color[1]);
            const BYTE bVal = static_cast<BYTE>(color[0]);
            const float luminance = (0.299f * rVal + 0.587f * gVal + 0.114f * bVal) / 255.0f;
            const bool isBrightColor = luminance > 0.72f;

            if (shape == tools3000::capture::NumberBadgeShape::RoundedSquare) {
                const float r = (std::min)(6.5f * scale, boxH * 0.45f);
                const float bx = cx - boxW * 0.5f;
                const float by = cy - boxH * 0.5f;

                Gdiplus::GraphicsPath squarePath;
                squarePath.AddArc(bx, by, r * 2.0f, r * 2.0f, 180.0f, 90.0f);
                squarePath.AddArc(bx + boxW - r * 2.0f, by, r * 2.0f, r * 2.0f, 270.0f, 90.0f);
                squarePath.AddArc(bx + boxW - r * 2.0f, by + boxH - r * 2.0f, r * 2.0f, r * 2.0f, 0.0f, 90.0f);
                squarePath.AddArc(bx, by + boxH - r * 2.0f, r * 2.0f, r * 2.0f, 90.0f, 90.0f);
                squarePath.CloseFigure();

                if (fill) {
                    Gdiplus::SolidBrush shadowBrush(Gdiplus::Color(95, 0, 0, 0));
                    g.TranslateTransform(1.5f * scale, 2.5f * scale);
                    g.FillPath(&shadowBrush, &squarePath);
                    g.ResetTransform();

                    Gdiplus::SolidBrush fillBrush(Gdiplus::Color(255, rVal, gVal, bVal));
                    g.FillPath(&fillBrush, &squarePath);

                    Gdiplus::Pen ringPen(isBrightColor ? Gdiplus::Color(75, 15, 23, 42) : Gdiplus::Color(220, 255, 255, 255), 1.2f * scale);
                    g.DrawPath(&ringPen, &squarePath);
                } else {
                    Gdiplus::SolidBrush bgBrush(isBrightColor ? Gdiplus::Color(220, 15, 23, 42) : Gdiplus::Color(235, 255, 255, 255));
                    g.FillPath(&bgBrush, &squarePath);

                    Gdiplus::Pen outlinePen(Gdiplus::Color(255, rVal, gVal, bVal), 2.2f * scale);
                    g.DrawPath(&outlinePen, &squarePath);
                }
            } else {
                if (fill) {
                    Gdiplus::SolidBrush shadowBrush(Gdiplus::Color(95, 0, 0, 0));
                    g.FillEllipse(&shadowBrush, cx - radius + 1.5f * scale, cy - radius + 2.5f * scale, radius * 2.0f, radius * 2.0f);

                    Gdiplus::SolidBrush circleBrush(Gdiplus::Color(255, rVal, gVal, bVal));
                    g.FillEllipse(&circleBrush, cx - radius, cy - radius, radius * 2.0f, radius * 2.0f);

                    Gdiplus::Pen ringPen(isBrightColor ? Gdiplus::Color(75, 15, 23, 42) : Gdiplus::Color(220, 255, 255, 255), 1.5f * scale);
                    g.DrawEllipse(&ringPen, cx - radius + 1.0f * scale, cy - radius + 1.0f * scale, (radius - 1.0f * scale) * 2.0f, (radius - 1.0f * scale) * 2.0f);
                } else {
                    Gdiplus::SolidBrush bgBrush(isBrightColor ? Gdiplus::Color(220, 15, 23, 42) : Gdiplus::Color(235, 255, 255, 255));
                    g.FillEllipse(&bgBrush, cx - radius, cy - radius, radius * 2.0f, radius * 2.0f);

                    Gdiplus::Pen outlinePen(Gdiplus::Color(255, rVal, gVal, bVal), 2.2f * scale);
                    g.DrawEllipse(&outlinePen, cx - radius + 1.0f * scale, cy - radius + 1.0f * scale, (radius - 1.0f * scale) * 2.0f, (radius - 1.0f * scale) * 2.0f);
                }
            }

            const Gdiplus::Color digitColor = fill
                ? (isBrightColor ? Gdiplus::Color(255, 15, 23, 42) : Gdiplus::Color(255, 255, 255, 255))
                : Gdiplus::Color(255, rVal, gVal, bVal);
            Gdiplus::SolidBrush numBrush(digitColor);

            if (pathBounds.Width > 0.0f && pathBounds.Height > 0.0f) {
                float fitScale = 1.0f;
                if (shape == tools3000::capture::NumberBadgeShape::Circle) {
                    const float safeInnerR = (std::max)(4.0f, radius - (fill ? 3.2f : 4.3f) * scale);
                    const float halfDiag = 0.5f * std::hypot(pathBounds.Width, pathBounds.Height);
                    if (halfDiag > safeInnerR) {
                        fitScale = std::clamp(safeInnerR / halfDiag, 0.50f, 1.0f);
                    }
                }
                const float pathCenterX = pathBounds.X + pathBounds.Width * 0.5f;
                const float pathCenterY = pathBounds.Y + pathBounds.Height * 0.5f;
                const float opticalOffsetX = (format == tools3000::capture::NumberFormat::Arabic && number == 1) ? (-0.35f * scale) : 0.0f;
                const float opticalOffsetY = -0.25f * scale;
                Gdiplus::Matrix transformMat;
                transformMat.Translate(cx + opticalOffsetX, cy + opticalOffsetY);
                if (std::abs(fitScale - 1.0f) > 0.01f) {
                    transformMat.Scale(fitScale, fitScale);
                }
                transformMat.Translate(-pathCenterX, -pathCenterY);
                textPath.Transform(&transformMat);
                g.FillPath(&numBrush, &textPath);
            }
        }

        const int startX = center.x - szW / 2;
        const int startY = center.y - szH / 2;
        blendOverlay(canvas, badgeMat, startX, startY);
    }

class MosaicHandler : public DefaultBoxHandler {
public:
    void render(cv::Mat& canvas, const MarkupElement& element) const override {
        int x1 = (std::min)(element.startPt.x, element.endPt.x);
        int y1 = (std::min)(element.startPt.y, element.endPt.y);
        int x2 = (std::max)(element.startPt.x, element.endPt.x);
        int y2 = (std::max)(element.startPt.y, element.endPt.y);

        x1 = (std::max)(0, x1); y1 = (std::max)(0, y1);
        x2 = (std::min)(canvas.cols, x2); y2 = (std::min)(canvas.rows, y2);

        const int bs = (std::max)(2, element.mosaicBlockSize);
        for (int yy = y1; yy < y2; yy += bs) {
            for (int xx = x1; xx < x2; xx += bs) {
                int bw = (std::min)(bs, x2 - xx);
                int bh = (std::min)(bs, y2 - yy);
                cv::Rect blockRect(xx, yy, bw, bh);
                cv::Mat block = canvas(blockRect);
                cv::Scalar meanColor = cv::mean(block);
                block.setTo(meanColor);
            }
        }
    }
};

class BlurHandler : public DefaultBoxHandler {
public:
    void render(cv::Mat& canvas, const MarkupElement& element) const override {
        int x1 = (std::min)(element.startPt.x, element.endPt.x);
        int y1 = (std::min)(element.startPt.y, element.endPt.y);
        int x2 = (std::max)(element.startPt.x, element.endPt.x);
        int y2 = (std::max)(element.startPt.y, element.endPt.y);

        x1 = (std::max)(0, x1); y1 = (std::max)(0, y1);
        x2 = (std::min)(canvas.cols, x2); y2 = (std::min)(canvas.rows, y2);

        if (x2 > x1 && y2 > y1) {
            cv::Rect roiRect(x1, y1, x2 - x1, y2 - y1);
            cv::Mat roi = canvas(roiRect);
            int ksize = element.mosaicBlockSize;
            if (ksize % 2 == 0) ksize += 1;
            if (ksize < 3) ksize = 3;
            int maxK = std::min(roi.cols, roi.rows);
            if (maxK % 2 == 0) maxK--;
            if (maxK >= 3) {
                if (ksize > maxK) ksize = maxK;
                try {
                    cv::GaussianBlur(roi, roi, cv::Size(ksize, ksize), 0, 0);
                } catch (...) {}
            } else if (roi.cols >= 2 || roi.rows >= 2) {
                try {
                    cv::blur(roi, roi, cv::Size(roi.cols, roi.rows));
                } catch (...) {}
            }
        }
    }
};

class TextHandler : public DefaultBoxHandler {
public:
    static int computeMinCustomBoxWidth(float fontSize) noexcept {
        const float safeFontSize = (std::max)(12.0f, fontSize);
        const int padW = tools3000::capture::CaptureTextEditor::computeBoxHorizontalPadding(safeFontSize);
        return (std::max)(44, static_cast<int>(std::ceil(safeFontSize * 1.4f)) + padW);
    }

    cv::Rect getBoundingBox(const MarkupElement& element) const override {
        if (element.textRenderSize.width <= 0 || element.textRenderSize.height <= 0) {
            std::wstring wtext = utf8ToWide(element.text);
            const_cast<MarkupElement&>(element).textRenderSize =
                measureSnipasteText(wtext, element.fontSize,
                                    element.isEditing || element.isActive,
                                    element.textMaxWidth,
                                    element.hasCustomTextWidth,
                                    element.customTextBoxHeight);
        }
        int bw = element.textRenderSize.width;
        int bh = element.textRenderSize.height;
        if (element.hasCustomTextWidth && element.textMaxWidth > 0.0f) {
            bw = (std::max)(bw, static_cast<int>(std::ceil(element.textMaxWidth)));
        }
        if (element.customTextBoxHeight > 0.0f) {
            bh = (std::max)(bh, static_cast<int>(std::ceil(element.customTextBoxHeight)));
        }
        if (element.text.empty()) {
            if (!element.hasCustomTextWidth) {
                int minEmptyW = static_cast<int>((std::max)(140.0f, element.fontSize * 5.0f));
                if (element.textMaxWidth > 0.0f) {
                    minEmptyW = (std::min)(minEmptyW, static_cast<int>(std::ceil(element.textMaxWidth)));
                }
                if (bw < minEmptyW) bw = (std::max)(32, minEmptyW);
            } else {
                const int minCustomW = computeMinCustomBoxWidth(element.fontSize);
                if (bw < minCustomW) bw = minCustomW;
            }
            if (bh < static_cast<int>(element.fontSize + 12)) bh = static_cast<int>((std::max)(28.0f, element.fontSize + 12.0f));
        } else {
            if (bw < 32) bw = static_cast<int>((std::max)(32.0f, element.fontSize * 1.5f));
            if (bh < static_cast<int>(element.fontSize * 1.2f)) bh = static_cast<int>((std::max)(24.0f, element.fontSize * 1.2f));
        }
        const_cast<MarkupElement&>(element).endPt = cv::Point(element.startPt.x + bw, element.startPt.y + bh);
        return cv::Rect(element.startPt.x, element.startPt.y, bw, bh);
    }

    HitArea hitTest(const MarkupElement& element, cv::Point pt, int padding) const override {
        const cv::Rect bbox = getBoundingBox(element);
        if (element.isActive) {
            const HitArea h = hitTestHandles(element, pt, padding);
            if (h != HitArea::None) return h;
        }
        const int p = (std::max)(padding, 6);
        const cv::Rect outerBox(bbox.x - p, bbox.y - p, bbox.width + p * 2, bbox.height + p * 2);
        if (!outerBox.contains(pt)) return HitArea::None;

        if (element.isEditing) {
            // 编辑态下：内部内容区用于点击定位光标与拖拽划词选中；外围边框环带用于直接拖动文本框位置
            const int insetX = (bbox.width > 18) ? 4 : 1;
            const int insetY = (bbox.height > 16) ? 3 : 1;
            const cv::Rect innerContent(bbox.x + insetX, bbox.y + insetY,
                                        std::max(1, bbox.width - insetX * 2),
                                        std::max(1, bbox.height - insetY * 2));
            return innerContent.contains(pt) ? HitArea::Body : HitArea::TextMoveBorder;
        }
        return HitArea::Body;
    }

    void resize(MarkupElement& element, int dx, int dy, HitArea handle) const override {
        if (handle == HitArea::TextMoveBorder) {
            element.moveBy(dx, dy);
            return;
        }
        const cv::Rect oldBox = getBoundingBox(element);
        const int oldW = (std::max)(1, oldBox.width);
        const int oldH = (std::max)(1, oldBox.height);

        // 1. 左右边中点手柄 (L / R)：独立调整文本框宽度并实时重排换行，严格保持字号不变且对侧边零漂移
        if (handle == HitArea::L || handle == HitArea::R) {
            const int minW = computeMinCustomBoxWidth(element.fontSize);
            constexpr int kMaxCustomW = 8192;
            const int rightEdge = oldBox.x + oldBox.width;
            const int targetW = (handle == HitArea::R) ? (oldW + dx) : (oldW - dx);
            const int clampedW = std::clamp(targetW, minW, kMaxCustomW);

            element.hasCustomTextWidth = true;
            element.textMaxWidth = static_cast<float>(clampedW);
            element.textRenderSize = cv::Size(0, 0);

            if (handle == HitArea::L) {
                element.startPt.x = rightEdge - clampedW;
            }
            const cv::Rect newBox = getBoundingBox(element);
            if (handle == HitArea::L) {
                element.startPt.x = rightEdge - newBox.width;
            }
            element.endPt = cv::Point(element.startPt.x + newBox.width, element.startPt.y + newBox.height);
            return;
        }

        // 2. 上下边中点手柄 (T / B)：独立拉长或收紧文本框高度，严格保持字号与宽度不变且对侧边零漂移
        if (handle == HitArea::T || handle == HitArea::B) {
            const std::wstring wtext = utf8ToWide(element.text);
            const cv::Size natSz = measureSnipasteText(
                wtext, element.fontSize,
                element.isEditing || element.isActive,
                element.textMaxWidth, element.hasCustomTextWidth, 0.0f);
            int natH = natSz.height;
            if (element.text.empty()) {
                natH = (std::max)(natH, static_cast<int>((std::max)(28.0f, element.fontSize + 12.0f)));
            } else {
                natH = (std::max)(natH, static_cast<int>((std::max)(24.0f, element.fontSize * 1.2f)));
            }

            constexpr int kMaxCustomH = 8192;
            const int bottomEdge = oldBox.y + oldBox.height;
            const int targetH = (handle == HitArea::B) ? (oldH + dy) : (oldH - dy);
            if (targetH <= natH + 2) {
                element.customTextBoxHeight = 0.0f;
            } else {
                element.customTextBoxHeight = static_cast<float>(std::clamp(targetH, natH, kMaxCustomH));
            }
            element.textRenderSize = cv::Size(0, 0);

            const cv::Rect newBox = getBoundingBox(element);
            if (handle == HitArea::T) {
                element.startPt.y = bottomEdge - newBox.height;
            }
            element.endPt = cv::Point(element.startPt.x + newBox.width, element.startPt.y + newBox.height);
            return;
        }

        // 3. 四角手柄 (LT / RT / LB / RB)：
        //    对角线正交最小二乘投影 + 一步正割闭环精修，确保在任意字号/宽高比下
        //    被拖拽角手柄 1:1 紧贴鼠标指针（零超前、零滞后、零整数阶跃、对侧角 0px 漂移）
        const float origFontSize = std::clamp(element.fontSize, 12.0f, 180.0f);
        const bool origHasCustomW = element.hasCustomTextWidth;
        const float origMaxW = element.textMaxWidth;
        const float origCustomH = element.customTextBoxHeight;
        const double w0 = static_cast<double>(oldW);
        const double h0 = static_cast<double>(oldH);
        const double signX = (handle == HitArea::RT || handle == HitArea::RB) ? 1.0 : -1.0;
        const double signY = (handle == HitArea::LB || handle == HitArea::RB) ? 1.0 : -1.0;
        const double desiredW = (std::max)(12.0, w0 + signX * static_cast<double>(dx));
        const double desiredH = (std::max)(12.0, h0 + signY * static_cast<double>(dy));
        const double diagSq = (std::max)(1.0, w0 * w0 + h0 * h0);
        const double targetProj = (desiredW * w0 + desiredH * h0) / diagSq;

        auto applyCandidateFontSize = [&](float candidateFont) -> cv::Rect {
            const float clampedF = std::clamp(candidateFont, 12.0f, 180.0f);
            element.fontSize = clampedF;
            const float ratio = (origFontSize > 1.0f) ? (clampedF / origFontSize) : 1.0f;
            if (origHasCustomW && origMaxW > 0.0f) {
                const float minScaledW = static_cast<float>(computeMinCustomBoxWidth(clampedF));
                element.textMaxWidth = std::clamp(origMaxW * ratio, minScaledW, 8192.0f);
            } else {
                element.textMaxWidth = origMaxW;
            }
            if (origCustomH > 0.0f) {
                element.customTextBoxHeight = std::clamp(
                    origCustomH * ratio,
                    (std::max)(24.0f, clampedF * 1.2f), 8192.0f);
            } else {
                element.customTextBoxHeight = 0.0f;
            }
            element.textRenderSize = cv::Size(0, 0);
            return getBoundingBox(element);
        };

        const float firstFont = std::clamp(
            static_cast<float>(static_cast<double>(origFontSize) * targetProj), 12.0f, 180.0f);
        cv::Rect newBox = applyCandidateFontSize(firstFont);

        // 一步正割校准：吸收内边距与字形度量带来的微小非线性偏差，使角点误差收敛至 <= 1px
        const double proj1 = (static_cast<double>(newBox.width) * w0 + static_cast<double>(newBox.height) * h0) / diagSq;
        const double diagLen = std::sqrt(diagSq);
        if (firstFont > 12.05f && firstFont < 179.95f &&
            std::abs(proj1 - targetProj) * diagLen > 1.5 &&
            std::abs(proj1 - 1.0) > 1e-4) {
            const double correctedScale = 1.0 + (firstFont / origFontSize - 1.0) * ((targetProj - 1.0) / (proj1 - 1.0));
            const float refinedFont = std::clamp(
                static_cast<float>(static_cast<double>(origFontSize) * correctedScale), 12.0f, 180.0f);
            if (std::abs(refinedFont - firstFont) >= 0.2f && std::abs(refinedFont - firstFont) <= origFontSize * 0.5f) {
                const cv::Rect candidateBox = applyCandidateFontSize(refinedFont);
                const double proj2 = (static_cast<double>(candidateBox.width) * w0 + static_cast<double>(candidateBox.height) * h0) / diagSq;
                if (std::abs(proj2 - targetProj) < std::abs(proj1 - targetProj)) {
                    newBox = candidateBox;
                } else {
                    newBox = applyCandidateFontSize(firstFont);
                }
            }
        }

        const int anchorRight = oldBox.x + oldBox.width;
        const int anchorBottom = oldBox.y + oldBox.height;
        int newX = oldBox.x;
        int newY = oldBox.y;
        if (handle == HitArea::LT || handle == HitArea::LB) {
            newX = anchorRight - newBox.width;
        }
        if (handle == HitArea::LT || handle == HitArea::RT) {
            newY = anchorBottom - newBox.height;
        }
        element.startPt = cv::Point(newX, newY);
        element.endPt = cv::Point(newX + newBox.width, newY + newBox.height);
    }

    void renderActiveHandles(cv::Mat& canvas, const MarkupElement& element) const override {
        const cv::Rect bbox = getBoundingBox(element);
        const cv::Scalar activeColor = element.isEditing
            ? cv::Scalar(246, 130, 59)   // 编辑态：亮蓝 (#3B82F6 in BGR)
            : cv::Scalar(255, 140, 0);   // 选中态：高辨识度科技蓝 (#008CFF in BGR)

        if (!element.isEditing) {
            cv::rectangle(canvas, cv::Rect(bbox.x - 1, bbox.y - 1, bbox.width + 2, bbox.height + 2),
                          cv::Scalar(255, 255, 255), 1, cv::LINE_AA);
            cv::rectangle(canvas, bbox, activeColor, 1, cv::LINE_AA);
        }

        // Figma / Keynote 级双语义视觉区分 + DPI 自适应尺寸：
        // - 四角手柄绘为圆形锚点（传达等比缩放字号）
        // - 左右/上下中点绘为方向胶囊短条（直观传达独立拉宽重排与拉长高度）
        const int cornerR = HandleGeometry::scaledHandleVisualHalfSize(element.dpiScale, element.isEditing ? 4 : 5);
        const int pillThickHalf = HandleGeometry::scaledHandleVisualHalfSize(element.dpiScale, 2);
        const int pillLenCap = HandleGeometry::scaledHandleVisualHalfSize(element.dpiScale, 7);
        const int pillLenMin = HandleGeometry::scaledHandleVisualHalfSize(element.dpiScale, 4);
        const auto handles = HandleGeometry::getBoxHandles(bbox);
        for (const auto& hp : handles) {
            if (hp.area == HitArea::L || hp.area == HitArea::R) {
                const int pillHalfH = (std::min)(pillLenCap, (std::max)(pillLenMin, bbox.height / 4));
                const cv::Rect pill(hp.point.x - pillThickHalf, hp.point.y - pillHalfH,
                                    pillThickHalf * 2 + 1, pillHalfH * 2 + 1);
                cv::rectangle(canvas, pill, cv::Scalar(255, 255, 255), cv::FILLED, cv::LINE_AA);
                cv::rectangle(canvas, pill, activeColor, 1, cv::LINE_AA);
            } else if (hp.area == HitArea::T || hp.area == HitArea::B) {
                const int pillHalfW = (std::min)(pillLenCap, (std::max)(pillLenMin, bbox.width / 4));
                const cv::Rect pill(hp.point.x - pillHalfW, hp.point.y - pillThickHalf,
                                    pillHalfW * 2 + 1, pillThickHalf * 2 + 1);
                cv::rectangle(canvas, pill, cv::Scalar(255, 255, 255), cv::FILLED, cv::LINE_AA);
                cv::rectangle(canvas, pill, activeColor, 1, cv::LINE_AA);
            } else {
                cv::circle(canvas, hp.point, cornerR + 1, cv::Scalar(255, 255, 255), cv::FILLED, cv::LINE_AA);
                cv::circle(canvas, hp.point, cornerR, activeColor, 1, cv::LINE_AA);
            }
        }
    }

    void render(cv::Mat& canvas, const MarkupElement& element) const override {
        auto color = element.color.toCvScalar();
        cv::Size renderedSize(0, 0);
        const bool effOutline = element.textOutline && (!element.hasCustomOutline || element.outlineThickness > 0.05f);
        const float outlineScale = element.hasCustomOutline ? (element.outlineThickness / 2.0f) : 1.0f;
        float effectiveMaxBoxWidth = element.textMaxWidth;
        if (!element.hasCustomTextWidth && effectiveMaxBoxWidth <= 0.0f && canvas.cols > 0) {
            effectiveMaxBoxWidth = tools3000::capture::CaptureTextEditor::computeAvailableBoxWidth(
                canvas.cols, element.startPt.x, element.fontSize);
            const_cast<MarkupElement&>(element).textMaxWidth = effectiveMaxBoxWidth;
        }
        renderSnipasteStyleText(canvas, element.text, element.startPt, color, element.fontSize,
                                element.isEditing, element.isActive, element.fill, renderedSize,
                                effOutline, element.textOutlineColor,
                                element.textCaret, element.textSelAnchor, element.textLastCaretTick,
                                outlineScale, effectiveMaxBoxWidth,
                                element.hasCustomTextWidth, element.customTextBoxHeight);
        const_cast<MarkupElement&>(element).textRenderSize = renderedSize;
        if (renderedSize.width > 0 && renderedSize.height > 0) {
            const_cast<MarkupElement&>(element).endPt = cv::Point(element.startPt.x + renderedSize.width, element.startPt.y + renderedSize.height);
        }
    }
};

class NumberHandler : public DefaultBoxHandler {
public:
    static float computeEffectiveScale(const MarkupElement& element) {
        const float baseDpi = element.dpiScale > 0.0f ? element.dpiScale : 1.0f;
        const float thick = element.thickness > 0.0f ? element.thickness : 2.0f;
        const float sizeFactor = std::clamp(0.76f + thick * 0.08f, 0.80f, 3.00f);
        return baseDpi * sizeFactor;
    }

    static std::pair<int, int> computeBadgeHalfExtents(const MarkupElement& element) {
        const float effScale = computeEffectiveScale(element);
        const int halfH = static_cast<int>(std::round(16.0f * effScale));
        int halfW = halfH;
        if (element.numberShape == NumberBadgeShape::RoundedSquare) {
            const std::wstring label = formatNumberBadgeLabel(element.numberValue, element.numberFormat);
            if (label.size() >= 2) {
                const float perChar = (element.numberFormat == NumberFormat::Chinese) ? 12.2f : 8.2f;
                const int estHalfW = static_cast<int>(std::ceil((label.size() * perChar + 14.0f) * effScale * 0.5f));
                halfW = (std::max)(halfH, estHalfW);
            }
        }
        return {halfW, halfH};
    }

    static cv::Point computeScaleHandlePt(const MarkupElement& element) {
        const float baseDpi = element.dpiScale > 0.0f ? element.dpiScale : 1.0f;
        const auto [halfW, halfH] = computeBadgeHalfExtents(element);
        const int pad = static_cast<int>(std::round(3.0f * baseDpi));
        // 若引出箭头恰好朝向右上角，则将缩放控制柄自动镜像至左上角，避免与引出虚线重叠
        int signX = 1;
        if (element.hasLeaderArrow) {
            const double adx = static_cast<double>(element.endPt.x - element.startPt.x);
            const double ady = static_cast<double>(element.endPt.y - element.startPt.y);
            if (adx > 0.0 && ady < 0.0) {
                signX = -1;
            }
        }
        if (element.numberShape == NumberBadgeShape::RoundedSquare && halfW > halfH) {
            return cv::Point(element.startPt.x + signX * (halfW + pad - 2),
                             element.startPt.y - (halfH + pad - 2));
        }
        constexpr double kCos45 = 0.70710678;
        const int haloR = halfH + pad - 1;
        return cv::Point(
            element.startPt.x + static_cast<int>(std::round(haloR * signX * kCos45)),
            element.startPt.y - static_cast<int>(std::round(haloR * kCos45)));
    }

    cv::Rect getBoundingBox(const MarkupElement& element) const override {
        const float effScale = computeEffectiveScale(element);
        const auto [halfW, halfH] = computeBadgeHalfExtents(element);
        cv::Rect badgeBox(element.startPt.x - halfW, element.startPt.y - halfH, halfW * 2, halfH * 2);
        if (element.hasLeaderArrow) {
            int pad = static_cast<int>(std::round(8.0f * effScale));
            int x1 = (std::min)(badgeBox.x, element.endPt.x - pad);
            int y1 = (std::min)(badgeBox.y, element.endPt.y - pad);
            int x2 = (std::max)(badgeBox.x + badgeBox.width, element.endPt.x + pad);
            int y2 = (std::max)(badgeBox.y + badgeBox.height, element.endPt.y + pad);
            return cv::Rect(x1, y1, x2 - x1, y2 - y1);
        }
        return badgeBox;
    }

    HitArea hitTest(const MarkupElement& element, cv::Point pt, int padding) const override {
        const auto [halfW, halfH] = computeBadgeHalfExtents(element);
        if (element.isActive) {
            const int handleHit = HandleGeometry::scaledHandleHitHalfSize(element.dpiScale, (std::max)(8, padding));
            if (element.hasLeaderArrow) {
                cv::Rect h2(element.endPt.x - handleHit, element.endPt.y - handleHit, handleHit * 2, handleHit * 2);
                if (h2.contains(pt)) return HitArea::RB; // 箭头尖端控制柄
            }
            const cv::Point scalePt = computeScaleHandlePt(element);
            if (std::hypot(pt.x - scalePt.x, pt.y - scalePt.y) <= static_cast<double>(handleHit)) {
                return HitArea::RT; // 外环缩放控制柄
            }
        }
        if (element.numberShape == NumberBadgeShape::RoundedSquare && halfW > halfH) {
            cv::Rect hitBox(element.startPt.x - halfW - padding,
                            element.startPt.y - halfH - padding,
                            (halfW + padding) * 2,
                            (halfH + padding) * 2);
            if (hitBox.contains(pt)) {
                return HitArea::Body;
            }
        } else {
            double distBadge = std::hypot(pt.x - element.startPt.x, pt.y - element.startPt.y);
            if (distBadge <= halfH + padding) {
                return HitArea::Body;
            }
        }
        if (element.hasLeaderArrow) {
            double dx = element.endPt.x - element.startPt.x;
            double dy = element.endPt.y - element.startPt.y;
            double lenSq = dx * dx + dy * dy;
            if (lenSq > 1e-6) {
                double t = std::clamp(((pt.x - element.startPt.x) * dx + (pt.y - element.startPt.y) * dy) / lenSq, 0.0, 1.0);
                double px = element.startPt.x + t * dx;
                double py = element.startPt.y + t * dy;
                double distLine = std::hypot(pt.x - px, pt.y - py);
                if (distLine <= (std::max)(static_cast<double>(padding), 8.0)) {
                    return HitArea::Body;
                }
            }
        }
        return HitArea::None;
    }

    void resize(MarkupElement& element, int dx, int dy, HitArea handle) const override {
        if (element.hasLeaderArrow && handle == HitArea::RB) {
            element.endPt.x += dx;
            element.endPt.y += dy;
        } else if (handle == HitArea::RT) {
            // 精确径向投影反解：使外环缩放控制柄在任意 DPI 与徽章形态下 1:1 紧贴鼠标指针
            const float baseDpi = element.dpiScale > 0.0f ? element.dpiScale : 1.0f;
            const auto [halfW, halfH] = computeBadgeHalfExtents(element);
            const cv::Point origHandle = computeScaleHandlePt(element);
            const double vx = static_cast<double>(origHandle.x - element.startPt.x);
            const double vy = static_cast<double>(origHandle.y - element.startPt.y);
            const double origDist = std::hypot(vx, vy);
            if (origDist > 1e-3) {
                const double tx = vx + static_cast<double>(dx);
                const double ty = vy + static_cast<double>(dy);
                const double projDist = (tx * vx + ty * vy) / origDist;
                const int pad = static_cast<int>(std::round(3.0f * baseDpi));
                double targetSizeFactor = 1.0;
                if (element.numberShape == NumberBadgeShape::RoundedSquare && halfW > halfH) {
                    const double origEffScale = static_cast<double>(computeEffectiveScale(element));
                    const double origSizeFactor = origEffScale / static_cast<double>(baseDpi);
                    const double constPadDiag = std::hypot(static_cast<double>(pad - 2), static_cast<double>(pad - 2));
                    const double origScalable = (std::max)(1.0, origDist - constPadDiag);
                    const double targetScalable = (std::max)(1.0, projDist - constPadDiag);
                    targetSizeFactor = origSizeFactor * (targetScalable / origScalable);
                } else {
                    const double targetHalfH = (std::max)(1.0, projDist - static_cast<double>(pad - 1));
                    targetSizeFactor = (targetHalfH / 16.0) / static_cast<double>(baseDpi);
                }
                const double targetThick = (targetSizeFactor - 0.76) / 0.08;
                element.thickness = std::clamp(static_cast<float>(targetThick), 1.0f, 28.0f);
            }
        } else {
            element.moveBy(dx, dy);
        }
    }

    void renderActiveHandles(cv::Mat& canvas, const MarkupElement& element) const override {
        cv::Scalar activeColor(255, 140, 0); // Azure/Blue
        const float baseDpi = element.dpiScale > 0.0f ? element.dpiScale : 1.0f;
        const auto [halfW, halfH] = computeBadgeHalfExtents(element);
        const int pad = static_cast<int>(std::round(3.0f * baseDpi));
        const int haloR = halfH + pad - 1;

        // 1. 在序号徽章外围绘制高亮轮廓，绝不在圆心绘制控制点遮挡字符
        if (element.numberShape == NumberBadgeShape::RoundedSquare && halfW > halfH) {
            cv::Rect haloRect(element.startPt.x - halfW - pad + 1,
                              element.startPt.y - halfH - pad + 1,
                              (halfW + pad - 1) * 2,
                              (halfH + pad - 1) * 2);
            cv::rectangle(canvas, haloRect, activeColor, 1, cv::LINE_AA);
        } else {
            cv::circle(canvas, element.startPt, haloR, activeColor, 1, cv::LINE_AA);
        }

        // 2. 在外环角部绘制无遮挡缩放控制柄，支持直接拖拽放大/缩小整个序号与字符
        const cv::Point scalePt = computeScaleHandlePt(element);
        const int scaleHandleR = HandleGeometry::scaledHandleVisualHalfSize(element.dpiScale, 5);
        cv::circle(canvas, scalePt, scaleHandleR, cv::Scalar(255, 255, 255), cv::FILLED, cv::LINE_AA);
        cv::circle(canvas, scalePt, scaleHandleR, activeColor, 1, cv::LINE_AA);

        if (!element.hasLeaderArrow) return;

        const double dx = static_cast<double>(element.endPt.x - element.startPt.x);
        const double dy = static_cast<double>(element.endPt.y - element.startPt.y);
        const double dist = std::hypot(dx, dy);
        if (dist > static_cast<double>(haloR + 4)) {
            const cv::Point lineStart(
                static_cast<int>(std::round(element.startPt.x + (dx / dist) * haloR)),
                static_cast<int>(std::round(element.startPt.y + (dy / dist) * haloR)));
            drawPatternedLine(canvas, lineStart, element.endPt, activeColor, 1, LineStyle::Dashed);
        }

        // 3. 在箭头尖端绘制可拖拽指向控制柄
        const int handleR = HandleGeometry::scaledHandleVisualHalfSize(element.dpiScale, 6);
        cv::circle(canvas, element.endPt, handleR, cv::Scalar(255, 255, 255), cv::FILLED, cv::LINE_AA);
        cv::circle(canvas, element.endPt, handleR, activeColor, 1, cv::LINE_AA);
    }

    void render(cv::Mat& canvas, const MarkupElement& element) const override {
        auto color = element.color.toCvScalar();
        const float effScale = computeEffectiveScale(element);
        int badgeR = static_cast<int>(std::round(15.0f * effScale));

        if (element.hasLeaderArrow) {
            // 箭身与箭头跟随序号徽章有效缩放倍率协调放大，避免箭身过粗而圆圈不放大
            int arrowThick = (std::max)(2, static_cast<int>(std::round(2.2f * effScale)));
            float dx = static_cast<float>(element.endPt.x - element.startPt.x);
            float dy = static_cast<float>(element.endPt.y - element.startPt.y);
            float dist = std::hypot(dx, dy);
            if (dist > static_cast<float>(badgeR)) {
                double angle = std::atan2(dy, dx);
                double headLen = (std::max)(11.0 * static_cast<double>(effScale), arrowThick * 4.2);
                if (headLen > dist * 0.45) headLen = dist * 0.45;
                double cA = std::cos(angle);
                double sA = std::sin(angle);
                double nX = -sA;
                double nY = cA;

                // 箭身起点位于序号徽章边缘
                cv::Point lineStart(
                    static_cast<int>(std::round(element.startPt.x + badgeR * cA)),
                    static_cast<int>(std::round(element.startPt.y + badgeR * sA))
                );

                // 经典实心尖角头部
                double halfW = headLen * 0.45;
                cv::Point basePt(static_cast<int>(std::round(element.endPt.x - headLen * cA)),
                                 static_cast<int>(std::round(element.endPt.y - headLen * sA)));
                cv::Point w1(static_cast<int>(std::round(basePt.x + halfW * nX)),
                             static_cast<int>(std::round(basePt.y + halfW * nY)));
                cv::Point w2(static_cast<int>(std::round(basePt.x - halfW * nX)),
                             static_cast<int>(std::round(basePt.y - halfW * nY)));
                std::vector<std::vector<cv::Point>> headPoly = { { element.endPt, w1, w2 } };
                cv::fillPoly(canvas, headPoly, color, cv::LINE_AA);

                drawPatternedLine(canvas, lineStart, basePt, color, arrowThick, element.lineStyle);
            }
        }

        renderSnipasteStyleNumberBadge(canvas, element.numberValue, element.startPt, color,
                                       element.fill, effScale, element.numberShape, element.numberFormat);
    }
};

class MagnifierHandler : public DefaultBoxHandler {
public:
    static cv::Point resolveLensCenter(const MarkupElement& element) {
        if (!element.hasLeaderArrow && element.endPt.x == 0 && element.endPt.y == 0 &&
            (element.startPt.x != 0 || element.startPt.y != 0)) {
            return element.startPt;
        }
        return element.endPt;
    }

    static bool isOffsetCallout(const MarkupElement& element) {
        const cv::Point lc = resolveLensCenter(element);
        return std::hypot(static_cast<double>(lc.x - element.startPt.x),
                          static_cast<double>(lc.y - element.startPt.y)) > 8.0;
    }

    static cv::Point computeFocusHandlePoint(const MarkupElement& element) {
        if (isOffsetCallout(element)) {
            return element.startPt;
        }
        const cv::Point lc = resolveLensCenter(element);
        const double rx = static_cast<double>(element.effectiveMagnifierRx());
        const double ry = static_cast<double>(element.effectiveMagnifierRy());
        // 同心模式下将“引出靶心手柄”置于透镜右上方约 -55° 椭圆弧线处，完全避开 T 与 RT 缩放手柄
        constexpr double kAngle = -55.0 * 3.14159265358979323846 / 180.0;
        return cv::Point(
            static_cast<int>(std::lround(lc.x + rx * std::cos(kAngle))),
            static_cast<int>(std::lround(lc.y + ry * std::sin(kAngle))));
    }

    cv::Rect getBoundingBox(const MarkupElement& element) const override {
        const cv::Point lc = resolveLensCenter(element);
        const int rx = element.effectiveMagnifierRx();
        const int ry = element.effectiveMagnifierRy();
        return cv::Rect(lc.x - rx, lc.y - ry, rx * 2, ry * 2);
    }

    HitArea hitTest(const MarkupElement& element, cv::Point pt, int padding) const override {
        const cv::Rect lensBox = getBoundingBox(element);
        const cv::Point lc = resolveLensCenter(element);
        const int rx = element.effectiveMagnifierRx();
        const int ry = element.effectiveMagnifierRy();
        const float dpi = (std::max)(1.0f, element.dpiScale);

        if (element.isActive) {
            const cv::Point focusPt = computeFocusHandlePoint(element);
            const bool offsetMode = isOffsetCallout(element);
            const int focusHitR = offsetMode
                ? static_cast<int>(std::lround(16.0f * dpi))
                : static_cast<int>(std::lround(11.0f * dpi));
            if (std::hypot(static_cast<double>(pt.x - focusPt.x),
                           static_cast<double>(pt.y - focusPt.y)) <= focusHitR) {
                // 同心模式下拖拽右上弧线青色引出手柄：保持源目标 startPt 在原位不动，将大透镜 endPt 引出至空白处！
                // 已分离引出模式下拖拽源中心靶点：独立移动采样源中心 startPt，大透镜留在空白处不动！
                return offsetMode ? HitArea::FocusSource : HitArea::LensOffset;
            }
            const int handleHalf = (std::max)({padding, HandleGeometry::kDefaultHandleHalfSize,
                                               static_cast<int>(std::lround(10.0f * dpi))});
            const HitArea h = HandleGeometry::hitTestHandles(lensBox, pt, handleHalf);
            if (h != HitArea::None) {
                return h;
            }
        }

        if (isOffsetCallout(element)) {
            const double centerHitR = (std::max)(14.0 * static_cast<double>(dpi), static_cast<double>(padding + 10));
            if (std::hypot(static_cast<double>(pt.x - element.startPt.x),
                           static_cast<double>(pt.y - element.startPt.y)) <= centerHitR) {
                return HitArea::FocusSource;
            }
        }

        bool insideLens = false;
        if (element.numberShape == NumberBadgeShape::RoundedSquare) {
            const cv::Rect padded(lensBox.x - padding, lensBox.y - padding,
                                  lensBox.width + padding * 2, lensBox.height + padding * 2);
            insideLens = padded.contains(pt);
        } else {
            const double arx = static_cast<double>((std::max)(1, rx + padding));
            const double ary = static_cast<double>((std::max)(1, ry + padding));
            const double nx = static_cast<double>(pt.x - lc.x) / arx;
            const double ny = static_cast<double>(pt.y - lc.y) / ary;
            insideLens = (nx * nx + ny * ny) <= 1.0;
        }

        if (insideLens) {
            return isOffsetCallout(element) ? HitArea::LensOffset : HitArea::Body;
        }
        return HitArea::None;
    }

    void resize(MarkupElement& element, int dx, int dy, HitArea handle) const override {
        if (handle == HitArea::FocusSource) {
            element.endPt = resolveLensCenter(element);
            element.startPt.x += dx;
            element.startPt.y += dy;
            element.hasLeaderArrow = isOffsetCallout(element);
            return;
        }
        if (handle == HitArea::LensOffset) {
            element.endPt = resolveLensCenter(element);
            element.endPt.x += dx;
            element.endPt.y += dy;
            element.hasLeaderArrow = isOffsetCallout(element);
            return;
        }

        const int rx = element.effectiveMagnifierRx();
        const int ry = element.effectiveMagnifierRy();
        switch (handle) {
            case HitArea::R:
                element.magnifierRadius = std::clamp(rx + dx, 20, 500);
                element.magnifierRadiusY = ry;
                break;
            case HitArea::L:
                element.magnifierRadius = std::clamp(rx - dx, 20, 500);
                element.magnifierRadiusY = ry;
                break;
            case HitArea::B:
                element.magnifierRadius = rx;
                element.magnifierRadiusY = std::clamp(ry + dy, 20, 500);
                break;
            case HitArea::T:
                element.magnifierRadius = rx;
                element.magnifierRadiusY = std::clamp(ry - dy, 20, 500);
                break;
            case HitArea::LT:
            case HitArea::RT:
            case HitArea::RB:
            case HitArea::LB: {
                const bool shiftHeld = (GetKeyState(VK_SHIFT) & 0x8000) != 0;
                if (shiftHeld) {
                    const int delta = HandleGeometry::computeScalarDelta(dx, dy, handle);
                    element.magnifierRadius = std::clamp(rx + delta, 20, 500);
                    element.magnifierRadiusY = std::clamp(ry + delta, 20, 500);
                } else {
                    const int signX = (handle == HitArea::RT || handle == HitArea::RB) ? 1 : -1;
                    const int signY = (handle == HitArea::LB || handle == HitArea::RB) ? 1 : -1;
                    element.magnifierRadius = std::clamp(rx + signX * dx, 20, 500);
                    element.magnifierRadiusY = std::clamp(ry + signY * dy, 20, 500);
                }
                break;
            }
            default: {
                const int delta = HandleGeometry::computeScalarDelta(dx, dy, handle);
                element.magnifierRadius = std::clamp(rx + delta, 20, 500);
                element.magnifierRadiusY = std::clamp(ry + delta, 20, 500);
                break;
            }
        }
    }

    void renderActiveHandles(cv::Mat& canvas, const MarkupElement& element) const override {
        const cv::Scalar activeBlue(246, 130, 59);  // BGR: 科技蓝
        const cv::Scalar focusCyan(212, 182, 6);    // BGR: 青翠高亮靶心
        const float sqrtDpi = std::sqrt((std::max)(1.0f, element.dpiScale));
        const int boxHandleR = (std::max)(5, static_cast<int>(std::lround(5.0f * sqrtDpi)));
        const int focusOuterR = (std::max)(7, static_cast<int>(std::lround(7.0f * sqrtDpi)));
        const int focusInnerR = (std::max)(2, static_cast<int>(std::lround(2.0f * sqrtDpi)));
        const cv::Rect lensBox = getBoundingBox(element);
        HandleGeometry::renderBoxHandles(canvas, lensBox, activeBlue, boxHandleR);

        // 绘制取景源靶心手柄（同心时位于右上弧线供一键拉出旁路放大，分离时位于被放大源正中心）
        const cv::Point focusPt = computeFocusHandlePoint(element);
        cv::circle(canvas, focusPt, focusOuterR, cv::Scalar(255, 255, 255), cv::FILLED, cv::LINE_AA);
        cv::circle(canvas, focusPt, focusOuterR, focusCyan, 2, cv::LINE_AA);
        cv::circle(canvas, focusPt, focusInnerR, focusCyan, cv::FILLED, cv::LINE_AA);
    }

    void render(cv::Mat& canvas, const MarkupElement& element) const override {
        const int rx = element.effectiveMagnifierRx();
        const int ry = element.effectiveMagnifierRy();
        const float zoom = std::clamp(element.magnifierScale, 1.1f, 8.0f);
        const int srcRx = static_cast<int>(std::lround(rx / zoom));
        const int srcRy = static_cast<int>(std::lround(ry / zoom));
        if (rx <= 4 || ry <= 4 || srcRx <= 1 || srcRy <= 1 || canvas.empty()) return;

        const cv::Point srcCenter = element.startPt;
        const cv::Point lensCenter = resolveLensCenter(element);
        const cv::Rect screenBounds(0, 0, canvas.cols, canvas.rows);

        // 1. 先从纯净未叠加本元素阴影/引线的画布等比无畸变采样源区域（边缘使用夹紧延展，消除黑边与比例失真）
        const cv::Rect srcRect(srcCenter.x - srcRx, srcCenter.y - srcRy, srcRx * 2, srcRy * 2);
        const cv::Rect dstRect(lensCenter.x - rx, lensCenter.y - ry, rx * 2, ry * 2);
        const cv::Rect validDst = dstRect & screenBounds;

        cv::Mat srcPatch(srcRy * 2, srcRx * 2, canvas.type(), cv::Scalar::all(28));
        const cv::Rect validSrc = srcRect & screenBounds;
        if (validSrc.area() > 0) {
            const int topPad = std::max(0, -srcRect.y);
            const int botPad = std::max(0, (srcRect.y + srcRect.height) - canvas.rows);
            const int leftPad = std::max(0, -srcRect.x);
            const int rightPad = std::max(0, (srcRect.x + srcRect.width) - canvas.cols);
            cv::copyMakeBorder(canvas(validSrc), srcPatch,
                               topPad, botPad, leftPad, rightPad,
                               cv::BORDER_REPLICATE);
        }

        // Lanczos-4 高阶 sinc 窗口重采样 + 3x3 局部极值包络钳制对比度自适应锐化 (Clamped CAS)，
        // 横纵轴保持相同 zoom 倍率无畸变放大，拉宽为椭圆时自动扩充视野而不拉扁文字
        cv::Mat enlarged;
        cv::resize(srcPatch, enlarged, cv::Size(rx * 2, ry * 2), 0, 0, cv::INTER_LANCZOS4);
        if (zoom >= 1.25f && std::min(rx, ry) >= 18) {
            cv::Mat softBlur, sharpened, localMin, localMax;
            cv::GaussianBlur(enlarged, softBlur, cv::Size(0, 0), 0.88);
            const double gain = std::clamp(0.58 + static_cast<double>(zoom - 1.5f) * 0.16, 0.45, 0.96);
            cv::addWeighted(enlarged, 1.0 + gain, softBlur, -gain, 0.0, sharpened);
            const cv::Mat k3 = cv::getStructuringElement(cv::MORPH_RECT, cv::Size(3, 3));
            cv::erode(enlarged, localMin, k3);
            cv::dilate(enlarged, localMax, k3);
            cv::subtract(localMin, cv::Scalar::all(3), localMin);
            cv::add(localMax, cv::Scalar::all(3), localMax);
            cv::max(sharpened, localMin, enlarged);
            cv::min(enlarged, localMax, enlarged);
        }

        ensureGdiplusStartup();
        const float dpi = element.dpiScale > 0.0f ? element.dpiScale : 1.0f;
        const float sqrtDpi = std::sqrt(dpi);
        const bool isRoundedSquare = (element.numberShape == NumberBadgeShape::RoundedSquare);
        const float minRf = static_cast<float>(std::min(rx, ry));
        const float cornerR = std::clamp(minRf * 0.28f, 8.0f * dpi, 32.0f * dpi);
        const float borderW = std::clamp((1.55f + element.thickness * 0.34f) * sqrtDpi, 2.2f, 4.8f * sqrtDpi);
        const float rimLum = (0.299f * element.color.r + 0.587f * element.color.g + 0.114f * element.color.b) / 255.0f;

        auto buildRoundedPath = [](Gdiplus::GraphicsPath& path, float x, float y, float w, float h, float cr) {
            const float d = std::min({cr * 2.0f, w, h});
            if (d <= 1.0f) {
                path.AddRectangle(Gdiplus::RectF(x, y, w, h));
                return;
            }
            path.AddArc(x, y, d, d, 180.0f, 90.0f);
            path.AddArc(x + w - d, y, d, d, 270.0f, 90.0f);
            path.AddArc(x + w - d, y + h - d, d, d, 0.0f, 90.0f);
            path.AddArc(x, y + h - d, d, d, 90.0f, 90.0f);
            path.CloseFigure();
        };

        auto drawJewelAnchor = [&](Gdiplus::Graphics& g, float ax, float ay, float jewelR) {
            Gdiplus::SolidBrush haloBr(Gdiplus::Color(135, 8, 12, 22));
            Gdiplus::SolidBrush mainBr(Gdiplus::Color(252, element.color.r, element.color.g, element.color.b));
            Gdiplus::SolidBrush coreBr(rimLum > 0.72f
                                           ? Gdiplus::Color(240, 30, 41, 59)
                                           : Gdiplus::Color(250, 255, 255, 255));
            const float hr = jewelR + 1.25f * sqrtDpi;
            const float cr = std::max(1.1f, jewelR * 0.45f);
            g.FillEllipse(&haloBr, ax - hr, ay - hr + 0.4f * sqrtDpi, hr * 2.0f, hr * 2.0f);
            g.FillEllipse(&mainBr, ax - jewelR, ay - jewelR, jewelR * 2.0f, jewelR * 2.0f);
            g.FillEllipse(&coreBr, ax - cr, ay - cr, cr * 2.0f, cr * 2.0f);
        };

        // 2. 学术期刊级多模态引出式详图系统 (Multi-Style Journal & Engineering Callout Loupe)：
        // 支持 4 种可选样式：
        //   - DashedDirect (默认):  ISO 128 精密虚线取景框 + 中心微十字 + 虚线直连
        //   - TangentFrustum:       源取景框 + 中心微十字 + 双外切展开细线 (CVPR / IEEE / Nature 经典局部放大)
        //   - ElbowLeader:          源取景框 + 中心微十字 + 135度工程折线引出 (Nature / Cell 解剖图)
        //   - CenterPointer:        单根流光细线 + 微燕尾箭头直指被放大正中心靶点
        const double calloutDist = std::hypot(static_cast<double>(lensCenter.x - srcCenter.x),
                                              static_cast<double>(lensCenter.y - srcCenter.y));
        bool hasLeaderBridge = false;
        float leaderUx = 0.0f;
        float leaderUy = 0.0f;
        float lensEdgeDist = minRf;

        const int srx = std::clamp(srcRx, 8, std::max(10, (rx * 2) / 3));
        const int sry = std::clamp(srcRy, 8, std::max(10, (ry * 2) / 3));
        const float srxf = static_cast<float>(srx);
        const float sryf = static_cast<float>(sry);
        const float srcCornerR = isRoundedSquare
            ? std::clamp(std::min(srxf, sryf) * 0.28f, 3.5f * sqrtDpi, 14.0f * sqrtDpi)
            : 0.0f;

        if (calloutDist > 8.0) {
            const int padCallout = std::max(16, static_cast<int>(std::ceil(18.0f * sqrtDpi)));
            const int minX = std::max(0, std::min(srcCenter.x - srx, lensCenter.x - rx) - padCallout);
            const int minY = std::max(0, std::min(srcCenter.y - sry, lensCenter.y - ry) - padCallout);
            const int maxX = std::min(canvas.cols, std::max(srcCenter.x + srx, lensCenter.x + rx) + padCallout);
            const int maxY = std::min(canvas.rows, std::max(srcCenter.y + sry, lensCenter.y + ry) + padCallout);
            if (maxX > minX && maxY > minY) {
                cv::Mat calloutLayer(maxY - minY, maxX - minX, CV_8UC4, cv::Scalar(0, 0, 0, 0));
                {
                    Gdiplus::Bitmap bmp(calloutLayer.cols, calloutLayer.rows,
                                        static_cast<INT>(calloutLayer.step),
                                        PixelFormat32bppARGB, calloutLayer.data);
                    Gdiplus::Graphics g(&bmp);
                    g.SetSmoothingMode(Gdiplus::SmoothingModeHighQuality);
                    g.SetPixelOffsetMode(Gdiplus::PixelOffsetModeHighQuality);

                    const float sx = static_cast<float>(srcCenter.x - minX) + 0.5f;
                    const float sy = static_cast<float>(srcCenter.y - minY) + 0.5f;
                    const float lx = static_cast<float>(lensCenter.x - minX) + 0.5f;
                    const float ly = static_cast<float>(lensCenter.y - minY) + 0.5f;
                    const float lrx = static_cast<float>(rx);
                    const float lry = static_cast<float>(ry);

                    const float dx = lx - sx;
                    const float dy = ly - sy;
                    const float d = std::hypot(dx, dy);
                    if (d > 1.0f) {
                        leaderUx = dx / d;
                        leaderUy = dy / d;
                        lensEdgeDist = computeLoupeBoundaryDistance(
                            -leaderUx, -leaderUy, lrx, lry, cornerR, isRoundedSquare);
                        const float srcEdgeDist = computeLoupeBoundaryDistance(
                            leaderUx, leaderUy, srxf, sryf, srcCornerR, isRoundedSquare);

                        const Gdiplus::Color haloColor = rimLum > 0.72f
                            ? Gdiplus::Color(155, 15, 23, 42)
                            : Gdiplus::Color(125, 8, 12, 22);
                        const Gdiplus::Color mainColor(250, element.color.r, element.color.g, element.color.b);

                        // 辅助函数：绘制源端真实采样取景框 + 正中心光学微十字准星 '+'
                        auto drawSourceApertureBox = [&](bool dashedBox) {
                            Gdiplus::Pen boxHaloPen(haloColor, 2.5f * sqrtDpi);
                            Gdiplus::Pen boxMainPen(mainColor, 1.35f * sqrtDpi);
                            const Gdiplus::REAL dashVals[2] = {4.0f, 3.0f};
                            if (dashedBox) {
                                boxHaloPen.SetDashPattern(dashVals, 2);
                                boxMainPen.SetDashPattern(dashVals, 2);
                            }
                            if (isRoundedSquare) {
                                Gdiplus::GraphicsPath sp;
                                buildRoundedPath(sp, sx - srxf, sy - sryf, srxf * 2.0f, sryf * 2.0f, srcCornerR);
                                g.DrawPath(&boxHaloPen, &sp);
                                g.DrawPath(&boxMainPen, &sp);
                            } else {
                                g.DrawEllipse(&boxHaloPen, sx - srxf, sy - sryf, srxf * 2.0f, sryf * 2.0f);
                                g.DrawEllipse(&boxMainPen, sx - srxf, sy - sryf, srxf * 2.0f, sryf * 2.0f);
                            }

                            // 中心光学微十字 '+'：让被放大的绝对几何中心一目了然，同时纤细不挡字
                            const float crossArm = std::clamp(std::min(srxf, sryf) * 0.24f, 3.0f * sqrtDpi, 5.2f * sqrtDpi);
                            Gdiplus::Pen crossHaloPen(haloColor, 2.1f * sqrtDpi);
                            Gdiplus::Pen crossMainPen(mainColor, 1.15f * sqrtDpi);
                            crossHaloPen.SetStartCap(Gdiplus::LineCapRound);
                            crossHaloPen.SetEndCap(Gdiplus::LineCapRound);
                            crossMainPen.SetStartCap(Gdiplus::LineCapRound);
                            crossMainPen.SetEndCap(Gdiplus::LineCapRound);
                            g.DrawLine(&crossHaloPen, sx - crossArm, sy, sx + crossArm, sy);
                            g.DrawLine(&crossHaloPen, sx, sy - crossArm, sx, sy + crossArm);
                            g.DrawLine(&crossMainPen, sx - crossArm, sy, sx + crossArm, sy);
                            g.DrawLine(&crossMainPen, sx, sy - crossArm, sx, sy + crossArm);
                        };

                        const MagnifierCalloutStyle calloutStyle = element.magnifierCalloutStyle;

                        if (calloutStyle == MagnifierCalloutStyle::CenterPointer) {
                            if (d > lensEdgeDist + 4.0f * sqrtDpi) {
                                hasLeaderBridge = true;
                                const Gdiplus::PointF pCenter(sx, sy);
                                const Gdiplus::PointF pLens(lx - leaderUx * lensEdgeDist, ly - leaderUy * lensEdgeDist);
                                const float spanLen = d - lensEdgeDist;

                                const float dirX = -leaderUx;
                                const float dirY = -leaderUy;
                                const float normX = -dirY;
                                const float normY = dirX;
                                const float pinR = 2.6f * sqrtDpi;
                                const float tipDistFromCenter = pinR + 0.8f * sqrtDpi;
                                const float headLen = std::clamp(spanLen * 0.32f, 7.5f * sqrtDpi, 11.5f * sqrtDpi);
                                const float headHalfW = headLen * 0.43f;
                                const bool showArrowhead = (spanLen > headLen + tipDistFromCenter + 4.0f * sqrtDpi);

                                const Gdiplus::PointF pShaftEnd = showArrowhead
                                    ? Gdiplus::PointF(sx - dirX * (tipDistFromCenter + headLen * 0.68f),
                                                      sy - dirY * (tipDistFromCenter + headLen * 0.68f))
                                    : pCenter;

                                Gdiplus::Pen shadowBridgePen(haloColor, 3.1f * sqrtDpi);
                                shadowBridgePen.SetStartCap(Gdiplus::LineCapRound);
                                shadowBridgePen.SetEndCap(Gdiplus::LineCapRound);
                                g.DrawLine(&shadowBridgePen, pLens, pShaftEnd);

                                Gdiplus::LinearGradientBrush bridgeGrad(
                                    pCenter, pLens,
                                    Gdiplus::Color(245, element.color.r, element.color.g, element.color.b),
                                    Gdiplus::Color(255, element.color.r, element.color.g, element.color.b));
                                Gdiplus::Pen mainBridgePen(&bridgeGrad, 1.85f * sqrtDpi);
                                mainBridgePen.SetStartCap(Gdiplus::LineCapRound);
                                mainBridgePen.SetEndCap(Gdiplus::LineCapRound);
                                g.DrawLine(&mainBridgePen, pLens, pShaftEnd);

                                if (showArrowhead) {
                                    const Gdiplus::PointF pTip(sx - dirX * tipDistFromCenter,
                                                               sy - dirY * tipDistFromCenter);
                                    const Gdiplus::PointF pBase(pTip.X - dirX * headLen,
                                                                pTip.Y - dirY * headLen);
                                    const Gdiplus::PointF pNotch(pTip.X - dirX * (headLen * 0.74f),
                                                                 pTip.Y - dirY * (headLen * 0.74f));
                                    const Gdiplus::PointF pWing1(pBase.X + normX * headHalfW,
                                                                 pBase.Y + normY * headHalfW);
                                    const Gdiplus::PointF pWing2(pBase.X - normX * headHalfW,
                                                                 pBase.Y - normY * headHalfW);
                                    Gdiplus::PointF arrowPts[4] = { pTip, pWing1, pNotch, pWing2 };
                                    Gdiplus::GraphicsPath arrowPath;
                                    arrowPath.AddPolygon(arrowPts, 4);

                                    Gdiplus::Pen arrowHaloPen(haloColor, 1.6f * sqrtDpi);
                                    arrowHaloPen.SetLineJoin(Gdiplus::LineJoinRound);
                                    g.DrawPath(&arrowHaloPen, &arrowPath);

                                    Gdiplus::SolidBrush arrowFillBr(mainColor);
                                    g.FillPath(&arrowFillBr, &arrowPath);
                                }

                                drawJewelAnchor(g, sx, sy, pinR);
                            }
                        } else if (calloutStyle == MagnifierCalloutStyle::ElbowLeader) {
                            drawSourceApertureBox(false);
                            if (d > lensEdgeDist + srcEdgeDist + 4.0f * sqrtDpi) {
                                const float signX = (dx >= 0.0f) ? 1.0f : -1.0f;
                                const float signY = (dy >= 0.0f) ? 1.0f : -1.0f;
                                const float diagUx = signX * 0.70710678f;
                                const float diagUy = signY * 0.70710678f;
                                const float srcDiagDist = computeLoupeBoundaryDistance(
                                    diagUx, diagUy, srxf, sryf, srcCornerR, isRoundedSquare);
                                const Gdiplus::PointF pSrcEdge(sx + diagUx * srcDiagDist, sy + diagUy * srcDiagDist);
                                const Gdiplus::PointF pLensEdge(lx - leaderUx * lensEdgeDist, ly - leaderUy * lensEdgeDist);

                                const float spanX = std::abs(pLensEdge.X - pSrcEdge.X);
                                const float spanY = std::abs(pLensEdge.Y - pSrcEdge.Y);
                                const float diagStep = std::min(spanX, spanY) * 0.65f;
                                const Gdiplus::PointF pElbow(pSrcEdge.X + signX * diagStep,
                                                             pSrcEdge.Y + signY * diagStep);

                                Gdiplus::PointF elbowPts[3] = { pSrcEdge, pElbow, pLensEdge };
                                Gdiplus::Pen haloPen(haloColor, 2.6f * sqrtDpi);
                                haloPen.SetStartCap(Gdiplus::LineCapRound);
                                haloPen.SetEndCap(Gdiplus::LineCapRound);
                                haloPen.SetLineJoin(Gdiplus::LineJoinRound);
                                g.DrawLines(&haloPen, elbowPts, 3);

                                Gdiplus::Pen linePen(mainColor, 1.45f * sqrtDpi);
                                linePen.SetStartCap(Gdiplus::LineCapRound);
                                linePen.SetEndCap(Gdiplus::LineCapRound);
                                linePen.SetLineJoin(Gdiplus::LineJoinRound);
                                g.DrawLines(&linePen, elbowPts, 3);
                            }
                        } else if (calloutStyle == MagnifierCalloutStyle::DashedDirect) {
                            drawSourceApertureBox(true);
                            if (d > lensEdgeDist + srcEdgeDist + 3.0f * sqrtDpi) {
                                const Gdiplus::PointF pSrcEdge(sx + leaderUx * srcEdgeDist, sy + leaderUy * srcEdgeDist);
                                const Gdiplus::PointF pLensEdge(lx - leaderUx * lensEdgeDist, ly - leaderUy * lensEdgeDist);
                                const Gdiplus::REAL dashVals[2] = {4.0f, 3.0f};

                                Gdiplus::Pen haloPen(haloColor, 2.5f * sqrtDpi);
                                haloPen.SetDashPattern(dashVals, 2);
                                haloPen.SetStartCap(Gdiplus::LineCapRound);
                                haloPen.SetEndCap(Gdiplus::LineCapRound);
                                g.DrawLine(&haloPen, pSrcEdge, pLensEdge);

                                Gdiplus::Pen linePen(mainColor, 1.35f * sqrtDpi);
                                linePen.SetDashPattern(dashVals, 2);
                                linePen.SetStartCap(Gdiplus::LineCapRound);
                                linePen.SetEndCap(Gdiplus::LineCapRound);
                                g.DrawLine(&linePen, pSrcEdge, pLensEdge);
                            }
                        } else {
                            // TangentFrustum：学术期刊双外切展开线（两侧精密外切细线 + 中间 100% 透明零色块污染）
                            drawSourceApertureBox(false);
                            if (d > lensEdgeDist + 2.0f * sqrtDpi) {
                                const float nx = -leaderUy;
                                const float ny = leaderUx;
                                const float rSmall = 0.5f * (srxf + sryf);
                                const float rLarge = 0.5f * (lrx + lry);
                                const float sinT = std::clamp((rLarge - rSmall) / std::max(d, 1.0f), -0.82f, 0.82f);
                                const float cosT = std::sqrt(std::max(0.05f, 1.0f - sinT * sinT));

                                Gdiplus::Pen tangentHaloPen(haloColor, 2.5f * sqrtDpi);
                                tangentHaloPen.SetStartCap(Gdiplus::LineCapRound);
                                tangentHaloPen.SetEndCap(Gdiplus::LineCapRound);

                                Gdiplus::LinearGradientBrush tangentGrad(
                                    Gdiplus::PointF(sx, sy), Gdiplus::PointF(lx, ly),
                                    Gdiplus::Color(215, element.color.r, element.color.g, element.color.b),
                                    Gdiplus::Color(250, element.color.r, element.color.g, element.color.b));
                                Gdiplus::Pen tangentMainPen(&tangentGrad, 1.35f * sqrtDpi);
                                tangentMainPen.SetStartCap(Gdiplus::LineCapRound);
                                tangentMainPen.SetEndCap(Gdiplus::LineCapRound);

                                for (float side : {-1.0f, 1.0f}) {
                                    const float vx = -sinT * leaderUx + side * cosT * nx;
                                    const float vy = -sinT * leaderUy + side * cosT * ny;
                                    const float sDist = computeLoupeBoundaryDistance(
                                        vx, vy, srxf, sryf, srcCornerR, isRoundedSquare);
                                    const float lDist = computeLoupeBoundaryDistance(
                                        vx, vy, lrx, lry, cornerR, isRoundedSquare);
                                    const Gdiplus::PointF pSrcSide(sx + vx * sDist, sy + vy * sDist);
                                    const Gdiplus::PointF pLensSide(lx + vx * lDist, ly + vy * lDist);
                                    g.DrawLine(&tangentHaloPen, pSrcSide, pLensSide);
                                    g.DrawLine(&tangentMainPen, pSrcSide, pLensSide);
                                }
                            }
                        }
                    }
                }
                blendOverlay(canvas, calloutLayer, minX, minY);
            }
        }

        if (validDst.area() <= 0) return;

        // 3. 构建独立高定蓝宝石光学透镜层（原生支持正圆与任意宽高比椭圆 / 圆角胶囊）
        const int pad = std::max(22, static_cast<int>(std::ceil(26.0f * sqrtDpi)));
        const int layerW = rx * 2 + pad * 2;
        const int layerH = ry * 2 + pad * 2;
        cv::Mat lensLayer(layerH, layerW, CV_8UC4, cv::Scalar(0, 0, 0, 0));
        {
            Gdiplus::Bitmap bmp(layerW, layerH, static_cast<INT>(lensLayer.step),
                                PixelFormat32bppARGB, lensLayer.data);
            Gdiplus::Graphics g(&bmp);
            g.SetSmoothingMode(Gdiplus::SmoothingModeHighQuality);
            g.SetPixelOffsetMode(Gdiplus::PixelOffsetModeHighQuality);
            g.SetTextRenderingHint(Gdiplus::TextRenderingHintAntiAlias);

            const float cx = static_cast<float>(pad + rx) + 0.5f;
            const float cy = static_cast<float>(pad + ry) + 0.5f;
            const float rxf = static_cast<float>(rx);
            const float ryf = static_cast<float>(ry);

            // 3.1 十层余弦衰减双模态立体悬浮投影（近距离高密度接触 AO + 远距离柔和高度漫射阴影）
            struct ShadowRing { float dy; float expand; BYTE alpha; };
            static constexpr ShadowRing kShadows[] = {
                { 1.0f,  0.8f, 48},
                { 2.0f,  1.8f, 40},
                { 3.2f,  3.2f, 32},
                { 4.6f,  4.8f, 25},
                { 6.2f,  6.8f, 19},
                { 8.0f,  9.0f, 14},
                { 9.8f, 11.4f, 10},
                {11.6f, 14.0f,  7},
                {13.4f, 16.8f,  4},
                {15.0f, 19.6f,  2},
            };
            for (const auto& sh : kShadows) {
                const float exp = sh.expand * sqrtDpi;
                const float dy = sh.dy * sqrtDpi;
                Gdiplus::SolidBrush sb(Gdiplus::Color(sh.alpha, 6, 10, 20));
                if (isRoundedSquare) {
                    Gdiplus::GraphicsPath sp;
                    buildRoundedPath(sp, cx - rxf - exp, cy - ryf - exp + dy,
                                     (rxf + exp) * 2.0f, (ryf + exp) * 2.0f, cornerR + exp);
                    g.FillPath(&sb, &sp);
                } else {
                    g.FillEllipse(&sb, cx - rxf - exp, cy - ryf - exp + dy,
                                  (rxf + exp) * 2.0f, (ryf + exp) * 2.0f);
                }
            }
        }

        // 3.2 使用浮点亚像素平滑蒙版将 Lanczos4+CAS 超清放大画面无缝嵌入 lensLayer 中心
        cv::Mat maskMat(ry * 2, rx * 2, CV_8UC4, cv::Scalar(0, 0, 0, 0));
        {
            Gdiplus::Bitmap mbmp(rx * 2, ry * 2, static_cast<INT>(maskMat.step),
                                 PixelFormat32bppARGB, maskMat.data);
            Gdiplus::Graphics mg(&mbmp);
            mg.SetSmoothingMode(Gdiplus::SmoothingModeHighQuality);
            mg.SetPixelOffsetMode(Gdiplus::PixelOffsetModeHighQuality);
            Gdiplus::SolidBrush opaqueWhite(Gdiplus::Color(255, 255, 255, 255));
            if (isRoundedSquare) {
                Gdiplus::GraphicsPath mp;
                buildRoundedPath(mp, 0.5f, 0.5f, rx * 2.0f - 1.0f, ry * 2.0f - 1.0f, cornerR);
                mg.FillPath(&opaqueWhite, &mp);
            } else {
                mg.FillEllipse(&opaqueWhite, 0.5f, 0.5f, rx * 2.0f - 1.0f, ry * 2.0f - 1.0f);
            }
        }

        cv::Mat enlargedBgra;
        if (enlarged.channels() == 3) {
            cv::cvtColor(enlarged, enlargedBgra, cv::COLOR_BGR2BGRA);
        } else {
            enlargedBgra = enlarged.clone();
        }

        for (int y = 0; y < ry * 2; ++y) {
            const auto* mRow = maskMat.ptr<cv::Vec4b>(y);
            const auto* sRow = enlargedBgra.ptr<cv::Vec4b>(y);
            auto* dRow = lensLayer.ptr<cv::Vec4b>(pad + y);
            for (int x = 0; x < rx * 2; ++x) {
                const uint8_t ma = mRow[x][3];
                if (ma == 0) continue;
                auto& dstPx = dRow[pad + x];
                if (ma == 255) {
                    dstPx = cv::Vec4b(sRow[x][0], sRow[x][1], sRow[x][2], 255);
                } else {
                    const float aS = ma / 255.0f;
                    const float aD = (dstPx[3] / 255.0f) * (1.0f - aS);
                    const float outA = aS + aD;
                    if (outA > 1e-4f) {
                        dstPx[0] = static_cast<uint8_t>(std::lround((sRow[x][0] * aS + dstPx[0] * aD) / outA));
                        dstPx[1] = static_cast<uint8_t>(std::lround((sRow[x][1] * aS + dstPx[1] * aD) / outA));
                        dstPx[2] = static_cast<uint8_t>(std::lround((sRow[x][2] * aS + dstPx[2] * aD) / outA));
                        dstPx[3] = static_cast<uint8_t>(std::clamp(std::lround(outA * 255.0f), 0L, 255L));
                    }
                }
            }
        }

        // 3.3 叠加内壁微凹景深、蓝宝石镀膜折射月牙光泽、三明治阳极氧化金属镜圈与透镜端宝石卡扣锚点
        {
            Gdiplus::Bitmap bmp(layerW, layerH, static_cast<INT>(lensLayer.step),
                                PixelFormat32bppARGB, lensLayer.data);
            Gdiplus::Graphics g(&bmp);
            g.SetSmoothingMode(Gdiplus::SmoothingModeHighQuality);
            g.SetPixelOffsetMode(Gdiplus::PixelOffsetModeHighQuality);
            g.SetTextRenderingHint(Gdiplus::TextRenderingHintAntiAlias);

            const float cx = static_cast<float>(pad + rx) + 0.5f;
            const float cy = static_cast<float>(pad + ry) + 0.5f;
            const float rxf = static_cast<float>(rx);
            const float ryf = static_cast<float>(ry);

            // (a) 光学凸透镜内壁微凹景深环 (Subtle Inner Rim Vignette)，消除边缘平贴感
            if (std::min(rx, ry) >= 24) {
                const float vigRx1 = std::max(6.0f, rxf - borderW * 0.55f - 1.2f);
                const float vigRy1 = std::max(6.0f, ryf - borderW * 0.55f - 1.2f);
                const float vigRx2 = std::max(5.0f, vigRx1 - 2.2f * sqrtDpi);
                const float vigRy2 = std::max(5.0f, vigRy1 - 2.2f * sqrtDpi);
                Gdiplus::Pen vigPen1(Gdiplus::Color(36, 8, 12, 24), 2.4f * sqrtDpi);
                Gdiplus::Pen vigPen2(Gdiplus::Color(15, 8, 12, 24), 2.6f * sqrtDpi);
                if (isRoundedSquare) {
                    Gdiplus::GraphicsPath vp1, vp2;
                    buildRoundedPath(vp1, cx - vigRx1, cy - vigRy1, vigRx1 * 2.0f, vigRy1 * 2.0f, std::max(3.0f, cornerR - 1.5f));
                    buildRoundedPath(vp2, cx - vigRx2, cy - vigRy2, vigRx2 * 2.0f, vigRy2 * 2.0f, std::max(2.0f, cornerR - 3.5f));
                    g.DrawPath(&vigPen1, &vp1);
                    g.DrawPath(&vigPen2, &vp2);
                } else {
                    g.DrawEllipse(&vigPen1, cx - vigRx1, cy - vigRy1, vigRx1 * 2.0f, vigRy1 * 2.0f);
                    g.DrawEllipse(&vigPen2, cx - vigRx2, cy - vigRy2, vigRx2 * 2.0f, vigRy2 * 2.0f);
                }
            }

            // (b) 顶部蓝宝石冷白镀膜折射高光弧（仅点亮最顶缘月牙区，中心 90% 阅读视野 100% 纯净无遮挡）
            if (std::min(rx, ry) >= 28) {
                Gdiplus::Pen glareOuterPen(Gdiplus::Color(56, 255, 255, 255), std::max(1.3f, 1.6f * sqrtDpi));
                glareOuterPen.SetStartCap(Gdiplus::LineCapRound);
                glareOuterPen.SetEndCap(Gdiplus::LineCapRound);
                if (!isRoundedSquare) {
                    const float grx = rxf - borderW * 0.85f - 1.8f * sqrtDpi;
                    const float gry = ryf - borderW * 0.85f - 1.8f * sqrtDpi;
                    if (grx > 8.0f && gry > 8.0f) {
                        g.DrawArc(&glareOuterPen, cx - grx, cy - gry, grx * 2.0f, gry * 2.0f, 214.0f, 112.0f);
                    }
                } else {
                    const float inset = borderW * 0.85f + 2.2f * sqrtDpi;
                    g.DrawLine(&glareOuterPen, cx - rxf + cornerR * 0.9f, cy - ryf + inset,
                               cx + rxf - cornerR * 0.9f, cy - ryf + inset);
                }
            }

            // (c) 倍率微晶角标（当 element.fill 开启且半径充裕时，在透镜底部内侧悬浮展示如 "2.0x"）
            if (element.fill && std::min(rx, ry) >= 34) {
                wchar_t zoomBuf[24]{};
                if (std::abs(zoom - std::round(zoom)) < 0.05f) {
                    swprintf_s(zoomBuf, L"%.0fx", zoom);
                } else {
                    swprintf_s(zoomBuf, L"%.1fx", zoom);
                }
                const float pillW = 36.0f * dpi;
                const float pillH = 18.0f * dpi;
                const float pillX = cx - pillW * 0.5f;
                const float pillY = cy + ryf - borderW - pillH - 4.5f * dpi;

                Gdiplus::GraphicsPath pillPath;
                buildRoundedPath(pillPath, pillX, pillY, pillW, pillH, pillH * 0.5f);
                Gdiplus::SolidBrush pillBg(Gdiplus::Color(186, 15, 23, 42));
                Gdiplus::Pen pillBorder(Gdiplus::Color(92, 255, 255, 255), 0.95f);
                g.FillPath(&pillBg, &pillPath);
                g.DrawPath(&pillBorder, &pillPath);

                Gdiplus::FontFamily fam(tools3000::capture::CaptureTextEditor::resolveMarkupFontFamilyName(true));
                Gdiplus::FontFamily fallbackFam(L"Microsoft YaHei UI");
                const Gdiplus::FontFamily* useFam = fam.IsAvailable() ? &fam : &fallbackFam;
                Gdiplus::Font pillFont(useFam, 12.0f * dpi, Gdiplus::FontStyleBold, Gdiplus::UnitPixel);
                Gdiplus::StringFormat sf;
                sf.SetAlignment(Gdiplus::StringAlignmentCenter);
                sf.SetLineAlignment(Gdiplus::StringAlignmentCenter);
                Gdiplus::SolidBrush textBr(Gdiplus::Color(248, 248, 250, 252));
                Gdiplus::RectF textRc(pillX, pillY, pillW, pillH);
                g.DrawString(zoomBuf, -1, &pillFont, textRc, &sf, &textBr);
            }

            // (d) 三明治阳极氧化精密金属镜圈：外层曜石分离细边 + 垂直受光微渐变金属主环 + 内层亮银/钛灰精密倒角细线
            Gdiplus::Pen outerContourPen(Gdiplus::Color(125, 8, 12, 22), borderW + 1.8f * sqrtDpi);
            const BYTE topR = static_cast<BYTE>(std::min(255, static_cast<int>(element.color.r) + 22));
            const BYTE topG = static_cast<BYTE>(std::min(255, static_cast<int>(element.color.g) + 22));
            const BYTE topB = static_cast<BYTE>(std::min(255, static_cast<int>(element.color.b) + 22));
            const BYTE botR = static_cast<BYTE>(std::max(0, static_cast<int>(element.color.r) - 18));
            const BYTE botG = static_cast<BYTE>(std::max(0, static_cast<int>(element.color.g) - 18));
            const BYTE botB = static_cast<BYTE>(std::max(0, static_cast<int>(element.color.b) - 18));
            Gdiplus::LinearGradientBrush bezelGrad(
                Gdiplus::PointF(cx, cy - ryf - 2.0f),
                Gdiplus::PointF(cx, cy + ryf + 2.0f),
                Gdiplus::Color(252, topR, topG, topB),
                Gdiplus::Color(252, botR, botG, botB));
            Gdiplus::Pen mainRimPen(&bezelGrad, borderW);
            Gdiplus::Pen innerRimPen(rimLum > 0.72f
                                         ? Gdiplus::Color(88, 15, 23, 42)
                                         : Gdiplus::Color(155, 255, 255, 255),
                                     0.95f * sqrtDpi);

            const float inRx = std::max(4.0f, rxf - borderW * 0.52f);
            const float inRy = std::max(4.0f, ryf - borderW * 0.52f);
            const float outRx = rxf + borderW * 0.45f;
            const float outRy = ryf + borderW * 0.45f;
            Gdiplus::Pen outerHairlinePen(Gdiplus::Color(55, 255, 255, 255), 0.75f);

            if (isRoundedSquare) {
                Gdiplus::GraphicsPath pMain, pInner, pOut;
                buildRoundedPath(pMain, cx - rxf, cy - ryf, rxf * 2.0f, ryf * 2.0f, cornerR);
                buildRoundedPath(pInner, cx - inRx, cy - inRy, inRx * 2.0f, inRy * 2.0f,
                                 std::max(2.0f, cornerR - borderW * 0.5f));
                buildRoundedPath(pOut, cx - outRx, cy - outRy, outRx * 2.0f, outRy * 2.0f, cornerR + borderW * 0.45f);
                g.DrawPath(&outerContourPen, &pMain);
                g.DrawPath(&mainRimPen, &pMain);
                g.DrawPath(&innerRimPen, &pInner);
                if (rimLum <= 0.72f) g.DrawPath(&outerHairlinePen, &pOut);
            } else {
                g.DrawEllipse(&outerContourPen, cx - rxf, cy - ryf, rxf * 2.0f, ryf * 2.0f);
                g.DrawEllipse(&mainRimPen, cx - rxf, cy - ryf, rxf * 2.0f, ryf * 2.0f);
                g.DrawEllipse(&innerRimPen, cx - inRx, cy - inRy, inRx * 2.0f, inRy * 2.0f);
                if (rimLum <= 0.72f) g.DrawEllipse(&outerHairlinePen, cx - outRx, cy - outRy, outRx * 2.0f, outRy * 2.0f);
            }

            // (e) 透镜端三层同心微宝石卡扣锚点（精准扣合在单轴引线与金属镜框的交接点，不被底层悬浮阴影压暗）
            if (hasLeaderBridge) {
                const float ax = cx - leaderUx * lensEdgeDist;
                const float ay = cy - leaderUy * lensEdgeDist;
                drawJewelAnchor(g, ax, ay, 2.9f * sqrtDpi);
            }
        }

        blendOverlay(canvas, lensLayer, lensCenter.x - rx - pad, lensCenter.y - ry - pad);
    }
};

class SpotlightHandler : public DefaultBoxHandler {
public:
    static void appendElementMask(cv::Mat& unionMask, const MarkupElement& element) {
        const int rawX1 = (std::min)(element.startPt.x, element.endPt.x);
        const int rawY1 = (std::min)(element.startPt.y, element.endPt.y);
        const int rawX2 = (std::max)(element.startPt.x, element.endPt.x);
        const int rawY2 = (std::max)(element.startPt.y, element.endPt.y);
        const int x1 = (std::max)(0, rawX1);
        const int y1 = (std::max)(0, rawY1);
        const int x2 = (std::min)(unionMask.cols, rawX2);
        const int y2 = (std::min)(unionMask.rows, rawY2);
        if (x2 <= x1 || y2 <= y1) return;

        const int w = x2 - x1;
        const int h = y2 - y1;
        cv::Rect roi(x1, y1, w, h);

        if (element.spotlightEllipse) {
            cv::Mat localMask = cv::Mat::zeros(h, w, CV_8UC1);
            const float fullW = static_cast<float>((std::max)(1, rawX2 - rawX1));
            const float fullH = static_cast<float>((std::max)(1, rawY2 - rawY1));
            const cv::Point center(
                static_cast<int>(std::lround((rawX1 + rawX2) * 0.5f - x1)),
                static_cast<int>(std::lround((rawY1 + rawY2) * 0.5f - y1)));
            const cv::Size axes(
                (std::max)(1, static_cast<int>(std::lround(fullW * 0.5f))),
                (std::max)(1, static_cast<int>(std::lround(fullH * 0.5f))));
            cv::ellipse(localMask, center, axes, 0, 0, 360, cv::Scalar(255), cv::FILLED, cv::LINE_AA);
            cv::Mat roiView = unionMask(roi);
            cv::max(roiView, localMask, roiView);
            return;
        }

        const float fullW = static_cast<float>(rawX2 - rawX1);
        const float fullH = static_cast<float>(rawY2 - rawY1);
        const float r = (std::min)({element.cornerRadius, fullW * 0.5f, fullH * 0.5f});
        if (r <= 0.5f) {
            unionMask(roi).setTo(255);
            return;
        }

        // 亚像素解析距离场圆角矩形并集蒙版（无锁极速抗锯齿）
        const float leftCx = static_cast<float>(rawX1) + r;
        const float rightCx = static_cast<float>(rawX2) - r;
        const float topCy = static_cast<float>(rawY1) + r;
        const float bottomCy = static_cast<float>(rawY2) - r;

        for (int y = y1; y < y2; ++y) {
            uchar* mRow = unionMask.ptr<uchar>(y);
            const float py = static_cast<float>(y) + 0.5f;
            const bool inCornerY = (py < topCy) || (py > bottomCy);
            const float cy = (py < topCy) ? topCy : bottomCy;
            if (!inCornerY) {
                std::memset(mRow + x1, 255, static_cast<std::size_t>(w));
                continue;
            }
            for (int x = x1; x < x2; ++x) {
                if (mRow[x] == 255) continue;
                const float px = static_cast<float>(x) + 0.5f;
                if (px >= leftCx && px <= rightCx) {
                    mRow[x] = 255;
                } else {
                    const float cx = (px < leftCx) ? leftCx : rightCx;
                    const float d = std::hypot(px - cx, py - cy);
                    if (d <= r - 0.5f) {
                        mRow[x] = 255;
                    } else if (d < r + 0.5f) {
                        const uint8_t v = static_cast<uint8_t>(std::lround(std::clamp(r + 0.5f - d, 0.0f, 1.0f) * 255.0f));
                        if (v > mRow[x]) mRow[x] = v;
                    }
                }
            }
        }
    }

    static void drawSpotlightBorder(cv::Mat& canvas, const MarkupElement& element) {
        if (!element.spotlightShowBorder && !element.fill) return;
        const int x1 = (std::min)(element.startPt.x, element.endPt.x);
        const int y1 = (std::min)(element.startPt.y, element.endPt.y);
        const int x2 = (std::max)(element.startPt.x, element.endPt.x);
        const int y2 = (std::max)(element.startPt.y, element.endPt.y);
        if (x2 <= x1 || y2 <= y1) return;

        const int thick = (std::max)(1, static_cast<int>(std::lround(element.thickness)));
        const cv::Scalar col = element.color.toCvScalar();
        auto drawStrokeOn = [&](cv::Mat& target, int ox, int oy) {
            const int lx1 = x1 - ox;
            const int ly1 = y1 - oy;
            const int lx2 = x2 - ox;
            const int ly2 = y2 - oy;
            if (element.spotlightEllipse) {
                const cv::Point center((lx1 + lx2) / 2, (ly1 + ly2) / 2);
                const cv::Size axes((std::max)(1, (lx2 - lx1) / 2), (std::max)(1, (ly2 - ly1) / 2));
                cv::ellipse(target, center, axes, 0, 0, 360, col, thick, cv::LINE_AA);
            } else {
                const float r = (std::min)({element.cornerRadius, (lx2 - lx1) * 0.5f, (ly2 - ly1) * 0.5f});
                if (r <= 0.5f) {
                    cv::rectangle(target, cv::Rect(lx1, ly1, lx2 - lx1, ly2 - ly1), col, thick, cv::LINE_AA);
                } else {
                    const int ir = static_cast<int>(std::lround(r));
                    cv::line(target, cv::Point(lx1 + ir, ly1), cv::Point(lx2 - ir, ly1), col, thick, cv::LINE_AA);
                    cv::line(target, cv::Point(lx1 + ir, ly2), cv::Point(lx2 - ir, ly2), col, thick, cv::LINE_AA);
                    cv::line(target, cv::Point(lx1, ly1 + ir), cv::Point(lx1, ly2 - ir), col, thick, cv::LINE_AA);
                    cv::line(target, cv::Point(lx2, ly1 + ir), cv::Point(lx2, ly2 - ir), col, thick, cv::LINE_AA);
                    cv::ellipse(target, cv::Point(lx1 + ir, ly1 + ir), cv::Size(ir, ir), 180, 0, 90, col, thick, cv::LINE_AA);
                    cv::ellipse(target, cv::Point(lx2 - ir, ly1 + ir), cv::Size(ir, ir), 270, 0, 90, col, thick, cv::LINE_AA);
                    cv::ellipse(target, cv::Point(lx2 - ir, ly2 - ir), cv::Size(ir, ir), 0, 0, 90, col, thick, cv::LINE_AA);
                    cv::ellipse(target, cv::Point(lx1 + ir, ly2 - ir), cv::Size(ir, ir), 90, 0, 90, col, thick, cv::LINE_AA);
                }
            }
        };

        if (element.color.a > 0 && element.color.a < 255) {
            const int pad = thick + 4;
            const cv::Rect roi = (cv::Rect(x1 - pad, y1 - pad, (x2 - x1) + pad * 2, (y2 - y1) + pad * 2) &
                                  cv::Rect(0, 0, canvas.cols, canvas.rows));
            if (roi.width > 0 && roi.height > 0) {
                const double alpha = std::clamp(static_cast<double>(element.color.a) / 255.0, 0.05, 1.0);
                cv::Mat dstRoi = canvas(roi);
                cv::Mat layerRoi = dstRoi.clone();
                drawStrokeOn(layerRoi, roi.x, roi.y);
                cv::addWeighted(layerRoi, alpha, dstRoi, 1.0 - alpha, 0.0, dstRoi);
            }
        } else {
            drawStrokeOn(canvas, 0, 0);
        }
    }

    static void renderUnified(cv::Mat& canvas, const std::vector<const MarkupElement*>& spotlights) {
        if (canvas.empty() || spotlights.empty()) return;
        const float dimAlpha = std::clamp(spotlights.back()->spotlightDimAlpha, 0.10f, 0.90f);
        LOG_DEBUG("标注引擎: 渲染聚光灯 dimAlpha={:.2f} ellipse={}",
                  dimAlpha, spotlights.back()->spotlightEllipse);

        cv::Mat unionMask = cv::Mat::zeros(canvas.size(), CV_8UC1);
        for (const MarkupElement* sp : spotlights) {
            if (sp) appendElementMask(unionMask, *sp);
        }

        const int keepDark256 = static_cast<int>(std::lround((1.0f - dimAlpha) * 256.0f));
        if (canvas.channels() == 3) {
            for (int y = 0; y < canvas.rows; ++y) {
                cv::Vec3b* row = canvas.ptr<cv::Vec3b>(y);
                const uchar* mRow = unionMask.ptr<uchar>(y);
                for (int x = 0; x < canvas.cols; ++x) {
                    const uchar m = mRow[x];
                    if (m == 255) continue;
                    if (m == 0) {
                        row[x][0] = static_cast<uchar>((row[x][0] * keepDark256) >> 8);
                        row[x][1] = static_cast<uchar>((row[x][1] * keepDark256) >> 8);
                        row[x][2] = static_cast<uchar>((row[x][2] * keepDark256) >> 8);
                    } else {
                        const float keep = 1.0f - dimAlpha * (1.0f - static_cast<float>(m) / 255.0f);
                        row[x][0] = static_cast<uchar>(std::lround(row[x][0] * keep));
                        row[x][1] = static_cast<uchar>(std::lround(row[x][1] * keep));
                        row[x][2] = static_cast<uchar>(std::lround(row[x][2] * keep));
                    }
                }
            }
        } else if (canvas.channels() == 4) {
            for (int y = 0; y < canvas.rows; ++y) {
                cv::Vec4b* row = canvas.ptr<cv::Vec4b>(y);
                const uchar* mRow = unionMask.ptr<uchar>(y);
                for (int x = 0; x < canvas.cols; ++x) {
                    const uchar m = mRow[x];
                    if (m == 255) continue;
                    if (m == 0) {
                        row[x][0] = static_cast<uchar>((row[x][0] * keepDark256) >> 8);
                        row[x][1] = static_cast<uchar>((row[x][1] * keepDark256) >> 8);
                        row[x][2] = static_cast<uchar>((row[x][2] * keepDark256) >> 8);
                    } else {
                        const float keep = 1.0f - dimAlpha * (1.0f - static_cast<float>(m) / 255.0f);
                        row[x][0] = static_cast<uchar>(std::lround(row[x][0] * keep));
                        row[x][1] = static_cast<uchar>(std::lround(row[x][1] * keep));
                        row[x][2] = static_cast<uchar>(std::lround(row[x][2] * keep));
                    }
                }
            }
        }

        for (const MarkupElement* sp : spotlights) {
            if (sp) drawSpotlightBorder(canvas, *sp);
        }
    }

    void render(cv::Mat& canvas, const MarkupElement& element) const override {
        renderUnified(canvas, {&element});
    }
};

class WatermarkHandler : public DefaultBoxHandler {
public:
    void render(cv::Mat& canvas, const MarkupElement& element) const override {
        LOG_DEBUG("标注引擎: 渲染水印 text='{}' opacity={:.2f} angle={:.1f} spacing={}",
                  element.watermarkText, element.watermarkOpacity, element.watermarkAngle, element.watermarkSpacing);

        if (element.watermarkText.empty()) return;

        int x1 = std::min(element.startPt.x, element.endPt.x);
        int y1 = std::min(element.startPt.y, element.endPt.y);
        int x2 = std::max(element.startPt.x, element.endPt.x);
        int y2 = std::max(element.startPt.y, element.endPt.y);
        x1 = std::max(0, x1); y1 = std::max(0, y1);
        x2 = std::min(canvas.cols, x2); y2 = std::min(canvas.rows, y2);
        if (x2 <= x1 || y2 <= y1) return;

        ensureGdiplusStartup();
        const int roiW = x2 - x1, roiH = y2 - y1;
        const float scale = element.dpiScale > 0.0f ? element.dpiScale : 1.0f;
        const float fontSize = std::clamp(16.0f * scale, 14.0f, 36.0f);
        const std::wstring wtext = utf8ToWide(element.watermarkText);

        if (isGdiplusStarted() && !wtext.empty()) {
            cv::Mat roiOverlay(roiH, roiW, CV_8UC4, cv::Scalar(0, 0, 0, 0));
            {
                Gdiplus::Bitmap bitmap(roiW, roiH, static_cast<INT>(roiOverlay.step), PixelFormat32bppARGB, roiOverlay.data);
                Gdiplus::Graphics g(&bitmap);
                g.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
                g.SetTextRenderingHint(Gdiplus::TextRenderingHintAntiAlias);

                Gdiplus::FontFamily fontFamily(tools3000::capture::CaptureTextEditor::resolveMarkupFontFamilyName(false));
                const Gdiplus::FontFamily* activeFamily = &fontFamily;
                Gdiplus::FontFamily fallbackFamily(L"Segoe UI");
                if (!fontFamily.IsAvailable()) {
                    activeFamily = &fallbackFamily;
                }
                Gdiplus::Font font(activeFamily, fontSize, Gdiplus::FontStyleBold, Gdiplus::UnitPixel);
                Gdiplus::StringFormat format(Gdiplus::StringFormat::GenericTypographic());
                Gdiplus::RectF boundRect;
                g.MeasureString(wtext.c_str(), -1, &font, Gdiplus::PointF(0.0f, 0.0f), &format, &boundRect);

                const float stepX = std::max(static_cast<float>(element.watermarkSpacing) * scale, boundRect.Width + 48.0f * scale);
                const float stepY = std::max(boundRect.Height + 48.0f * scale, stepX * 0.55f);
                const float diag = std::hypot(static_cast<float>(roiW), static_cast<float>(roiH));

                const BYTE alphaByte = static_cast<BYTE>(std::clamp(element.watermarkOpacity * 255.0f, 24.0f, 220.0f));
                Gdiplus::SolidBrush wmBrush(Gdiplus::Color(alphaByte, 220, 226, 236));
                Gdiplus::SolidBrush shadowBrush(Gdiplus::Color(static_cast<BYTE>(alphaByte * 0.55f), 15, 23, 42));

                g.TranslateTransform(roiW * 0.5f, roiH * 0.5f);
                g.RotateTransform(element.watermarkAngle);

                int rowIdx = 0;
                for (float y = -diag; y <= diag; y += stepY, ++rowIdx) {
                    const float xOffset = (rowIdx % 2 != 0) ? (stepX * 0.5f) : 0.0f;
                    for (float x = -diag + xOffset; x <= diag; x += stepX) {
                        g.DrawString(wtext.c_str(), -1, &font, Gdiplus::PointF(x + 1.0f, y + 1.0f), &format, &shadowBrush);
                        g.DrawString(wtext.c_str(), -1, &font, Gdiplus::PointF(x, y), &format, &wmBrush);
                    }
                }
            }
            blendOverlay(canvas, roiOverlay, x1, y1);
            return;
        }

        int baseline = 0;
        const double fontScale = 0.6;
        const cv::Size textSz = cv::getTextSize(
            element.watermarkText, cv::FONT_HERSHEY_SIMPLEX, fontScale, 1, &baseline);
        if (textSz.width <= 0 || textSz.height <= 0) return;

        // 仅在紧凑单印章缓冲 (几十 KB) 上渲染并旋转水印文字，再按网格周期平铺混合至目标 ROI，
        // 彻底杜绝 (roiW^2 + roiH^2) 32 位整数溢出与超长图 10GB 巨型旋转矩阵 OOM
        const int stampW = std::clamp(textSz.width + 12, 8, 2048);
        const int stampH = std::clamp(textSz.height + baseline + 12, 8, 1024);
        cv::Mat stampSrc(stampH, stampW, CV_8UC4, cv::Scalar(0, 0, 0, 0));
        cv::putText(stampSrc, element.watermarkText, cv::Point(6, 6 + textSz.height),
                    cv::FONT_HERSHEY_SIMPLEX, fontScale, cv::Scalar(200, 200, 200, 255), 1, cv::LINE_AA);

        const double angleRad = static_cast<double>(element.watermarkAngle) * (3.14159265358979323846 / 180.0);
        const double absCos = std::abs(std::cos(angleRad));
        const double absSin = std::abs(std::sin(angleRad));
        const int rotW = (std::max)(8, static_cast<int>(std::ceil(stampW * absCos + stampH * absSin)) + 4);
        const int rotH = (std::max)(8, static_cast<int>(std::ceil(stampW * absSin + stampH * absCos)) + 4);

        const cv::Point2f srcCenter(static_cast<float>(stampW) * 0.5f, static_cast<float>(stampH) * 0.5f);
        cv::Mat rotMat = cv::getRotationMatrix2D(srcCenter, element.watermarkAngle, 1.0);
        rotMat.at<double>(0, 2) += static_cast<double>(rotW) * 0.5 - static_cast<double>(srcCenter.x);
        rotMat.at<double>(1, 2) += static_cast<double>(rotH) * 0.5 - static_cast<double>(srcCenter.y);

        cv::Mat stampRot;
        cv::warpAffine(stampSrc, stampRot, rotMat, cv::Size(rotW, rotH),
                       cv::INTER_LINEAR, cv::BORDER_CONSTANT, cv::Scalar(0, 0, 0, 0));

        const int spacing = (std::max)(element.watermarkSpacing, textSz.width + 20);
        const int vSpacing = (std::max)(16, textSz.height + spacing / 2);
        const float wmOpacity = std::clamp(element.watermarkOpacity, 0.0f, 1.0f);
        if (wmOpacity <= 0.001f) return;

        const double cosA = std::cos(-angleRad);
        const double sinA = std::sin(-angleRad);
        const double halfRoiW = static_cast<double>(roiW) * 0.5;
        const double halfRoiH = static_cast<double>(roiH) * 0.5;
        const double maxRadius = std::hypot(halfRoiW + rotW, halfRoiH + rotH);
        const int maxGridX = static_cast<int>(std::ceil(maxRadius / static_cast<double>(spacing))) + 1;
        const int maxGridY = static_cast<int>(std::ceil(maxRadius / static_cast<double>(vSpacing))) + 1;

        for (int gy = -maxGridY; gy <= maxGridY; ++gy) {
            const double py = static_cast<double>(gy) * static_cast<double>(vSpacing);
            for (int gx = -maxGridX; gx <= maxGridX; ++gx) {
                const double px = static_cast<double>(gx) * static_cast<double>(spacing);
                const int cx = static_cast<int>(std::lround(halfRoiW + px * cosA - py * sinA));
                const int cy = static_cast<int>(std::lround(halfRoiH + px * sinA + py * cosA));
                const int drawX = cx - rotW / 2;
                const int drawY = cy - rotH / 2;
                if (drawX + rotW <= 0 || drawY + rotH <= 0 || drawX >= roiW || drawY >= roiH) {
                    continue;
                }
                const int sx0 = (std::max)(0, -drawX);
                const int sy0 = (std::max)(0, -drawY);
                const int sx1 = (std::min)(rotW, roiW - drawX);
                const int sy1 = (std::min)(rotH, roiH - drawY);
                for (int sy = sy0; sy < sy1; ++sy) {
                    const int canvasY = y1 + drawY + sy;
                    const auto* srcRow = stampRot.ptr<cv::Vec4b>(sy);
                    if (canvas.channels() == 4) {
                        auto* dstRow = canvas.ptr<cv::Vec4b>(canvasY);
                        for (int sx = sx0; sx < sx1; ++sx) {
                            const uint8_t a = srcRow[sx][3];
                            if (a == 0) continue;
                            const float alpha = (static_cast<float>(a) / 255.0f) * wmOpacity;
                            auto& pxDst = dstRow[x1 + drawX + sx];
                            pxDst[0] = static_cast<uchar>(srcRow[sx][0] * alpha + pxDst[0] * (1.0f - alpha));
                            pxDst[1] = static_cast<uchar>(srcRow[sx][1] * alpha + pxDst[1] * (1.0f - alpha));
                            pxDst[2] = static_cast<uchar>(srcRow[sx][2] * alpha + pxDst[2] * (1.0f - alpha));
                        }
                    } else if (canvas.channels() == 3) {
                        auto* dstRow = canvas.ptr<cv::Vec3b>(canvasY);
                        for (int sx = sx0; sx < sx1; ++sx) {
                            const uint8_t a = srcRow[sx][3];
                            if (a == 0) continue;
                            const float alpha = (static_cast<float>(a) / 255.0f) * wmOpacity;
                            auto& pxDst = dstRow[x1 + drawX + sx];
                            pxDst[0] = static_cast<uchar>(srcRow[sx][0] * alpha + pxDst[0] * (1.0f - alpha));
                            pxDst[1] = static_cast<uchar>(srcRow[sx][1] * alpha + pxDst[1] * (1.0f - alpha));
                            pxDst[2] = static_cast<uchar>(srcRow[sx][2] * alpha + pxDst[2] * (1.0f - alpha));
                        }
                    }
                }
            }
        }
    }
};

class InpaintHandler : public DefaultBoxHandler {
public:
    void render(cv::Mat& canvas, const MarkupElement& element) const override {
        const double safeRadius = std::clamp(static_cast<double>(element.inpaintRadius), 1.0, 32.0);
        LOG_DEBUG("标注引擎: 渲染智能消除 radius={}", static_cast<int>(safeRadius));

        int x1 = std::min(element.startPt.x, element.endPt.x);
        int y1 = std::min(element.startPt.y, element.endPt.y);
        int x2 = std::max(element.startPt.x, element.endPt.x);
        int y2 = std::max(element.startPt.y, element.endPt.y);
        x1 = std::max(0, x1); y1 = std::max(0, y1);
        x2 = std::min(canvas.cols, x2); y2 = std::min(canvas.rows, y2);
        if (x2 <= x1 || y2 <= y1) return;

        // 仅在目标选区外扩邻域子块 (Local Padded ROI) 上执行 TELEA 图像修复，避免全屏大图转换与全图扫描开销
        const int pad = std::max(16, static_cast<int>(std::ceil(safeRadius * 3.0)));
        const int rx1 = std::max(0, x1 - pad);
        const int ry1 = std::max(0, y1 - pad);
        const int rx2 = std::min(canvas.cols, x2 + pad);
        const int ry2 = std::min(canvas.rows, y2 + pad);
        const cv::Rect subRect(rx1, ry1, rx2 - rx1, ry2 - ry1);
        const cv::Rect innerRect(x1 - rx1, y1 - ry1, x2 - x1, y2 - y1);

        try {
            cv::Mat subCanvas = canvas(subRect);
            cv::Mat mask = cv::Mat::zeros(subCanvas.size(), CV_8UC1);
            cv::rectangle(mask, innerRect, cv::Scalar(255), cv::FILLED);

            cv::Mat bgr;
            if (subCanvas.channels() == 4) {
                cv::cvtColor(subCanvas, bgr, cv::COLOR_BGRA2BGR);
            } else if (subCanvas.channels() == 3) {
                bgr = subCanvas;
            } else if (subCanvas.channels() == 1) {
                cv::cvtColor(subCanvas, bgr, cv::COLOR_GRAY2BGR);
            } else {
                return;
            }

            cv::Mat result;
            cv::inpaint(bgr, mask, result, safeRadius, cv::INPAINT_TELEA);

            cv::Rect dstRoi(x1, y1, x2 - x1, y2 - y1);
            if (canvas.channels() == 4) {
                cv::Mat result4;
                cv::cvtColor(result(innerRect), result4, cv::COLOR_BGR2BGRA);
                result4.copyTo(canvas(dstRoi));
            } else if (canvas.channels() == 3) {
                result(innerRect).copyTo(canvas(dstRoi));
            }
        } catch (const cv::Exception& e) {
            LOG_WARN("MarkupEngine: OpenCV inpaint skipped due to exception: {}", e.what());
        }
    }
};

class TextReplaceHandler : public IMarkupToolHandler {
public:
    void render(cv::Mat& canvas, const MarkupElement& element) const override {
        const cv::Rect srcRc = (element.replaceSourceRect.width > 0 && element.replaceSourceRect.height > 0)
            ? element.replaceSourceRect
            : cv::Rect(element.startPt, element.endPt);
        if (srcRc.width <= 0 || srcRc.height <= 0) return;

        TextReplaceRenderSpec spec{};
        spec.sourceRect = srcRc;
        spec.charBoxes = element.replaceCharBoxes;
        spec.originalTextUtf8 = element.replaceOriginalText;
        spec.newTextUtf8 = element.text;
        spec.textBgr = cv::Vec3b(element.color.b, element.color.g, element.color.r);
        spec.fontSizePx = element.fontSize;
        spec.fontWeight = element.replaceFontWeight;
        spec.fontClass = static_cast<TextReplaceFontClass>(std::clamp(element.replaceFontClass, 0, 2));
        spec.offsetX = element.replaceOffsetX;
        spec.offsetY = element.replaceOffsetY;
        spec.bgMode = static_cast<TextReplaceBgMode>(std::clamp(element.replaceBgMode, 0, 2));
        spec.styleInitialized = element.replaceStyleInitialized;
        spec.reflowRightTail = element.replaceReflowRight;
        spec.rightTailRect = element.replaceRightTailRect;
        spec.isEditing = element.isEditing;
        spec.caretUtf16 = element.textCaret;
        spec.selAnchorUtf16 = element.textSelAnchor;
        spec.lastCaretTick = element.textLastCaretTick;
        spec.showCaretBlink = true;

        const cv::Rect rendered = TextReplaceEngine::renderReplacement(canvas, spec);
        if (rendered.width > 0 && rendered.height > 0) {
            element.replaceLastRenderRect = rendered;
        }
    }

    cv::Rect getBoundingBox(const MarkupElement& element) const override {
        if (element.replaceLastRenderRect.width > 0 && element.replaceLastRenderRect.height > 0) {
            return element.replaceLastRenderRect;
        }
        return (element.replaceSourceRect.width > 0 && element.replaceSourceRect.height > 0)
            ? element.replaceSourceRect
            : cv::Rect(element.startPt, element.endPt);
    }

    HitArea hitTest(const MarkupElement& element, cv::Point pt, int padding) const override {
        cv::Rect box = getBoundingBox(element);
        if (box.width <= 0 || box.height <= 0) return HitArea::None;
        cv::Rect padded(box.x - padding, box.y - padding, box.width + padding * 2, box.height + padding * 2);
        return padded.contains(pt) ? HitArea::Body : HitArea::None;
    }

    void resize(MarkupElement& element, int dx, int dy, HitArea /*handle*/) const override {
        element.replaceOffsetX += static_cast<float>(dx);
        element.replaceOffsetY += static_cast<float>(dy);
    }

    void renderActiveHandles(cv::Mat& canvas, const MarkupElement& element) const override {
        if (element.isEditing) return;
        const cv::Rect box = getBoundingBox(element);
        if (box.width > 0 && box.height > 0) {
            cv::rectangle(canvas, box, cv::Scalar(246, 130, 59), 1, cv::LINE_AA);
        }
    }
};

} // namespace

namespace markup_detail {
void renderUnifiedSpotlights(cv::Mat& canvas, const std::vector<const MarkupElement*>& spotlights) {
    SpotlightHandler::renderUnified(canvas, spotlights);
}

void registerEffectMarkupHandlers(MarkupToolRegistry& registry) {
    registry.registerHandler(MarkupTool::Mosaic, std::make_shared<MosaicHandler>());
    registry.registerHandler(MarkupTool::Blur, std::make_shared<BlurHandler>());
    registry.registerHandler(MarkupTool::Text, std::make_shared<TextHandler>());
    registry.registerHandler(MarkupTool::Number, std::make_shared<NumberHandler>());
    registry.registerHandler(MarkupTool::Magnifier, std::make_shared<MagnifierHandler>());
    registry.registerHandler(MarkupTool::Spotlight, std::make_shared<SpotlightHandler>());
    registry.registerHandler(MarkupTool::Watermark, std::make_shared<WatermarkHandler>());
    registry.registerHandler(MarkupTool::Inpaint, std::make_shared<InpaintHandler>());
    registry.registerHandler(MarkupTool::TextReplace, std::make_shared<TextReplaceHandler>());
}
} // namespace markup_detail

} // namespace tools3000::capture
