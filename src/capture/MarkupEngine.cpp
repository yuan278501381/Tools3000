// ─────────────────────────────────────────────────────────────────────────────
// MarkupEngine.cpp — 截图标注引擎实现
//
// 所有标注元素使用 OpenCV 绘制:
//   - 矩形: cv::rectangle
//   - 箭头: cv::arrowedLine
//   - 椭圆: cv::ellipse
//   - 画笔: cv::polylines
//   - 高亮: cv::Mat ROI + alpha blending
//   - 马赛克: 区域像素块化
//   - 文本: cv::putText
//   - 序号: 圆形背景 + 数字
//   - 放大镜: ROI 放大 + 圆形裁剪
//   - 聚光灯: 半透明黑色遮罩 + 选区孔洞
//   - 水印: 旋转文字平铺 + alpha 混合
//   - 智能消除: cv::inpaint (TELEA) 背景重建
// ─────────────────────────────────────────────────────────────────────────────

#ifndef NOMINMAX
#define NOMINMAX
#endif

#include "capture/MarkupEngine.h"
#include "capture/CaptureInteractionPolicy.h"
#include "capture/CaptureTextEditor.h"
#include "capture/CornerRadiusHelper.h"
#include "core/logger/Logger.h"
#include "core/utils/DWriteFontUtils.h"
#include "core/utils/TraceId.h"

#include <algorithm>
#include <windows.h>

#include <objidl.h>
#include <gdiplus.h>
#pragma comment(lib, "gdiplus.lib")
#include <opencv2/imgproc.hpp>
#include <opencv2/photo.hpp>
#include <cmath>
#include <limits>
#include <mutex>

namespace tools3000::capture {

float sampleCanvasRegionLuminance(const cv::Mat& canvas, const cv::Rect& roiRect) {
    if (canvas.empty() || (canvas.channels() != 3 && canvas.channels() != 4)) {
        return 0.50f;
    }
    const cv::Rect bounds(0, 0, canvas.cols, canvas.rows);
    cv::Rect clipped = roiRect & bounds;
    if (clipped.width <= 0 || clipped.height <= 0) {
        if (roiRect.width > 0 && roiRect.height > 0) {
            const int w = std::min(roiRect.width, canvas.cols);
            const int h = std::min(roiRect.height, canvas.rows);
            const int cx = std::clamp(roiRect.x, 0, std::max(0, canvas.cols - w));
            const int cy = std::clamp(roiRect.y, 0, std::max(0, canvas.rows - h));
            clipped = cv::Rect(cx, cy, w, h) & bounds;
        }
        if (clipped.width <= 0 || clipped.height <= 0) {
            clipped = bounds;
        }
    }
    const cv::Scalar meanVal = cv::mean(canvas(clipped));
    const float b = static_cast<float>(meanVal[0]);
    const float g = static_cast<float>(meanVal[1]);
    const float r = static_cast<float>(meanVal[2]);
    const float lum = (0.299f * r + 0.587f * g + 0.114f * b) / 255.0f;
    return std::clamp(lum, 0.0f, 1.0f);
}

MarkupColor computeAdaptiveTextOutlineColor(const MarkupColor& textColor, float bgLuminance) {
    const float textLum = (0.299f * static_cast<float>(textColor.r) +
                           0.587f * static_cast<float>(textColor.g) +
                           0.114f * static_cast<float>(textColor.b)) / 255.0f;
    const MarkupColor darkOutline{15, 17, 23, 250};
    const MarkupColor lightOutline{255, 255, 255, 250};

    // 1. 极深色文字（如极客黑 #1E293B / 纯黑 #000000，亮度 < 0.22）：
    //    若使用深色描边会导致描边与字芯完全融为一团黑影，且在深色背景上彻底不可见，因此恒定采用纯白高反差描边
    if (textLum < 0.22f) {
        return lightOutline;
    }

    // 2. 极亮浅色/纯白文字（如纯白 #FFFFFF 或高明度浅粉彩，亮度 > 0.78）：
    //    若使用白色描边会导致文字边缘发胖糊化，且在浅色背景上不可见，因此恒定采用曜石黑高反差描边
    if (textLum > 0.78f) {
        return darkOutline;
    }

    // 3. 彩色及中等亮度文字（红/橙/黄/绿/蓝/紫等，0.22 <= textLum <= 0.78）：
    //    严格基于底层背景亮度自动反色：深色背景 (bgLuminance < 0.50f) 配纯白高光描边，浅色背景配曜石黑沉稳描边
    return (bgLuminance < 0.50f) ? lightOutline : darkOutline;
}

MarkupColor resolveTextOutlineColor(const MarkupColor& textColor,
                                    const MarkupColor& outlineColor,
                                    const cv::Mat& canvas,
                                    const cv::Rect& textRect,
                                    bool withBackdrop) {
    if (!outlineColor.isAuto()) {
        const uint8_t a = outlineColor.a > 0 ? outlineColor.a : 250;
        return MarkupColor{outlineColor.r, outlineColor.g, outlineColor.b, a};
    }

    float bgLum = sampleCanvasRegionLuminance(canvas, textRect);
    if (withBackdrop) {
        const float textLum = (0.299f * static_cast<float>(textColor.r) +
                               0.587f * static_cast<float>(textColor.g) +
                               0.114f * static_cast<float>(textColor.b)) / 255.0f;
        const float capsuleLum = (textLum > 0.45f) ? 0.08f : 0.96f;
        const float capsuleAlpha = (textLum > 0.45f) ? (190.0f / 255.0f) : (200.0f / 255.0f);
        bgLum = bgLum * (1.0f - capsuleAlpha) + capsuleLum * capsuleAlpha;
    }
    return computeAdaptiveTextOutlineColor(textColor, bgLum);
}

} // namespace tools3000::capture

namespace {
    std::vector<cv::Point> generateSmoothSpline(const std::vector<cv::Point>& pts, int stepsPerSegment = 10) {
        if (pts.size() < 3) return pts;

        std::vector<cv::Point> cleanPts;
        cleanPts.reserve(pts.size());
        for (const auto& p : pts) {
            if (cleanPts.empty() || cleanPts.back() != p) {
                cleanPts.push_back(p);
            }
        }
        if (cleanPts.size() < 3) return pts;

        std::vector<cv::Point> extended(cleanPts.size() + 2);
        extended[0] = cleanPts[0] - (cleanPts[1] - cleanPts[0]);
        for (size_t i = 0; i < cleanPts.size(); ++i) extended[i + 1] = cleanPts[i];
        extended[extended.size() - 1] = cleanPts.back() + (cleanPts.back() - cleanPts[cleanPts.size() - 2]);

        std::vector<cv::Point> smoothPts;
        smoothPts.reserve((cleanPts.size() - 1) * stepsPerSegment + 1);

        for (size_t i = 1; i < extended.size() - 2; ++i) {
            cv::Point2f p0 = extended[i - 1], p1 = extended[i], p2 = extended[i + 1], p3 = extended[i + 2];
            const int limit = (i == extended.size() - 3) ? (stepsPerSegment + 1) : stepsPerSegment;
            for (int t_i = 0; t_i < limit; ++t_i) {
                float t = static_cast<float>(t_i) / stepsPerSegment;
                float t2 = t * t;
                float t3 = t2 * t;
                float x = 0.5f * ((2.0f * p1.x) + (-p0.x + p2.x) * t + (2.0f * p0.x - 5.0f * p1.x + 4.0f * p2.x - p3.x) * t2 + (-p0.x + 3.0f * p1.x - 3.0f * p2.x + p3.x) * t3);
                float y = 0.5f * ((2.0f * p1.y) + (-p0.y + p2.y) * t + (2.0f * p0.y - 5.0f * p1.y + 4.0f * p2.y - p3.y) * t2 + (-p0.y + 3.0f * p1.y - 3.0f * p2.y + p3.y) * t3);
                cv::Point pt(static_cast<int>(std::round(x)), static_cast<int>(std::round(y)));
                if (smoothPts.empty() || smoothPts.back() != pt) {
                    smoothPts.push_back(pt);
                }
            }
        }
        if (smoothPts.size() == 1) {
            smoothPts.push_back(cleanPts.back());
        }
        return smoothPts;
    }

    std::mutex g_gdiPlusMutex;
    ULONG_PTR g_gdiPlusToken = 0;

    void ensureGdiplusStartup() {
        std::lock_guard lock(g_gdiPlusMutex);
        if (g_gdiPlusToken != 0) return;
        Gdiplus::GdiplusStartupInput input;
        ULONG_PTR token = 0;
        if (Gdiplus::GdiplusStartup(&token, &input, nullptr) == Gdiplus::Ok) {
            g_gdiPlusToken = token;
        }
    }

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
                        bg[0] = static_cast<uchar>(fg[0] * alpha + bg[0] * (1.0f - alpha));
                        bg[1] = static_cast<uchar>(fg[1] * alpha + bg[1] * (1.0f - alpha));
                        bg[2] = static_cast<uchar>(fg[2] * alpha + bg[2] * (1.0f - alpha));
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
                        bg[0] = static_cast<uchar>(fg[0] * alpha + bg[0] * (1.0f - alpha));
                        bg[1] = static_cast<uchar>(fg[1] * alpha + bg[1] * (1.0f - alpha));
                        bg[2] = static_cast<uchar>(fg[2] * alpha + bg[2] * (1.0f - alpha));
                        bg[3] = 255;
                    }
                }
            }
        }
    }
    cv::Size measureSnipasteText(const std::wstring& text, int fontSize, bool /*isEditing*/ = false) {
        ensureGdiplusStartup();
        const int safeFontSize = (std::max)(12, fontSize);
        const auto [originX, originY] = tools3000::capture::CaptureTextEditor::computeTextOrigin(safeFontSize);
        const int padW = static_cast<int>(std::ceil(originX * 2.0f + 4.0f));
        const int padH = static_cast<int>(std::ceil(originY * 2.0f + 2.0f));
        if (g_gdiPlusToken == 0) {
            int w = text.empty() ? (124 + padW) : static_cast<int>((std::max)(40.0f, safeFontSize * 2.0f)) + padW;
            int h = static_cast<int>((std::max)(24.0f, safeFontSize * 1.3f)) + padH;
            return cv::Size((std::max)(w, 24 + padW), (std::max)(h, safeFontSize + padH));
        }

        Gdiplus::FontFamily fontFamily(tools3000::capture::CaptureTextEditor::resolveMarkupFontFamilyName(false));
        const Gdiplus::FontFamily* activeFamily = &fontFamily;
        Gdiplus::FontFamily fallbackFamily(L"Segoe UI");
        if (!fontFamily.IsAvailable()) {
            activeFamily = &fallbackFamily;
        }
        Gdiplus::Font font(activeFamily, static_cast<Gdiplus::REAL>(safeFontSize), Gdiplus::FontStyleBold, Gdiplus::UnitPixel);
        Gdiplus::StringFormat format(Gdiplus::StringFormat::GenericTypographic());
        format.SetAlignment(Gdiplus::StringAlignmentNear);
        format.SetLineAlignment(Gdiplus::StringAlignmentNear);
        format.SetFormatFlags(format.GetFormatFlags() | Gdiplus::StringFormatFlagsMeasureTrailingSpaces);

        Gdiplus::Bitmap tmpBmp(1, 1, PixelFormat32bppARGB);
        Gdiplus::Graphics g(&tmpBmp);
        Gdiplus::RectF boundRect;
        const std::wstring placeholder = tools3000::core::isUiLanguageChinese()
            ? L"点击输入文字..."
            : L"Click to enter text...";
        std::wstring measureStr = text.empty() ? placeholder : text;
        if (!measureStr.empty() && measureStr.back() == L'\n') {
            // GDI+ MeasureString 会忽略末尾单独的换行符，附加占位空格以准确计算新空行高度
            measureStr.push_back(L' ');
        }
        g.MeasureString(measureStr.c_str(), -1, &font, Gdiplus::PointF(0, 0), &format, &boundRect);
        int w = static_cast<int>(std::ceil(boundRect.Width)) + padW;
        int h = static_cast<int>(std::ceil(boundRect.Height)) + padH;
        return cv::Size((std::max)(w, 24), (std::max)(h, safeFontSize + padH));
    }

    void renderSnipasteStyleText(cv::Mat& canvas, const std::string& text, cv::Point pt, const cv::Scalar& color,
                                 int fontSize, bool isEditing, bool isSelected, bool withBackdrop, cv::Size& outSize,
                                 bool textOutline = true,
                                 const tools3000::capture::MarkupColor& outlineColor = tools3000::capture::MarkupColor::Auto(),
                                 size_t caretIndex = 0, size_t selAnchor = 0, uint32_t lastCaretTick = 0,
                                 float outlineThicknessScale = 1.0f) {
        ensureGdiplusStartup();
        std::wstring wtext = utf8ToWide(text);
        if (wtext.empty() && !isEditing && !isSelected) {
            outSize = cv::Size(0, 0);
            return;
        }

        cv::Size sz = measureSnipasteText(wtext, fontSize, isEditing || isSelected);
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

            const int safeFontSize = (std::max)(12, fontSize);
            const auto [textX, textY] = tools3000::capture::CaptureTextEditor::computeTextOrigin(safeFontSize);

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
            }

            tools3000::capture::MultilineTextLayout editLayout;
            if (isEditing) {
                editLayout = tools3000::capture::CaptureTextEditor::buildLayout(wtext, fontSize);
                // 绘制多行文本选区高亮微晶底块（支持鼠标拖拽划词与 Ctrl+A 全选）
                if (tools3000::capture::CaptureTextEditor::hasSelection(caretIndex, selAnchor) && !wtext.empty()) {
                    const auto spans = tools3000::capture::CaptureTextEditor::computeSelectionHighlights(
                        editLayout, wtext, caretIndex, selAnchor);
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
                Gdiplus::GraphicsPath textPath;
                textPath.AddString(wtext.c_str(), -1, activeFamily, Gdiplus::FontStyleBold, static_cast<Gdiplus::REAL>(safeFontSize), Gdiplus::PointF(textX, textY), &format);

                // 文字高精度外扩描边轮廓渲染 (先绘制外扩描边轮廓，再绘制内芯文字，确保高反差清晰度)
                if (textOutline) {
                    const tools3000::capture::MarkupColor textColor{rVal, gVal, bVal, 255};
                    const tools3000::capture::MarkupColor resolvedOutline =
                        tools3000::capture::resolveTextOutlineColor(
                            textColor, outlineColor, canvas,
                            cv::Rect(pt.x, pt.y, sz.width, sz.height), withBackdrop);
                    const BYTE a = resolvedOutline.a > 0 ? resolvedOutline.a : 250;
                    Gdiplus::Color strokeColor(a, resolvedOutline.r, resolvedOutline.g, resolvedOutline.b);
                    const float strokeWidth = tools3000::capture::CaptureTextEditor::computeOutlineStrokeWidth(safeFontSize) *
                                              std::clamp(outlineThicknessScale, 0.25f, 4.0f);
                    Gdiplus::Pen strokePen(strokeColor, strokeWidth);
                    strokePen.SetLineJoin(Gdiplus::LineJoinRound);
                    g.DrawPath(&strokePen, &textPath);
                }

                // 正文矢量文字填充
                Gdiplus::SolidBrush textBrush(Gdiplus::Color(255, rVal, gVal, bVal));
                g.FillPath(&textBrush, &textPath);
            } else if (isEditing || isSelected) {
                // 空文本编辑态：绘制提示占位文字（严格遵循字重 >= 500 Bold 与字号 >= 12px 排版红线）
                const std::wstring placeholder = tools3000::core::isUiLanguageChinese()
                    ? L"点击输入文字..."
                    : L"Click to enter text...";
                Gdiplus::Font placeholderFont(activeFamily, static_cast<Gdiplus::REAL>((std::max)(12.0f, safeFontSize * 0.88f)), Gdiplus::FontStyleBold, Gdiplus::UnitPixel);
                Gdiplus::SolidBrush placeholderBrush(Gdiplus::Color(180, 148, 163, 184));
                g.DrawString(placeholder.c_str(), -1, &placeholderFont, Gdiplus::PointF(textX + 2.0f, textY + 1.0f), &format, &placeholderBrush);
            }

            // 2. 编辑态光标绘制（支持任意行/列精确定位，且在移动光标或输入后的 520ms 内保持常亮）
            if (isEditing) {
                const uint32_t nowTick = GetTickCount();
                const bool recentlyMoved = (lastCaretTick != 0) && ((nowTick - lastCaretTick) < 520u);
                const bool showCursor = recentlyMoved || ((nowTick / 480u) % 2u == 0u) || wtext.empty();
                if (showCursor) {
                    const auto caretVis = tools3000::capture::CaptureTextEditor::computeCaretVisual(
                        editLayout, wtext, caretIndex);
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

        Gdiplus::GraphicsPath textPath;
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
}

#include <algorithm>
#include <cmath>
#include <atomic>
#include <chrono>

namespace tools3000::capture {

cv::Mat applyRoundedCorners(const cv::Mat& src, float radius) {
    if (src.empty() || radius <= 0.5f) {
        return src.clone();
    }

    const int w = src.cols;
    const int h = src.rows;
    const float r = (std::min)({radius, static_cast<float>(w) * 0.5f, static_cast<float>(h) * 0.5f});
    if (r <= 0.5f) {
        return src.clone();
    }

    cv::Mat dst;
    if (src.channels() == 4) {
        dst = src.clone();
    } else if (src.channels() == 3) {
        cv::cvtColor(src, dst, cv::COLOR_BGR2BGRA);
    } else {
        return src.clone();
    }

    const int intR = static_cast<int>(std::ceil(r));

    auto applyCorner = [&](int startX, int endX, int startY, int endY, float centerX, float centerY) {
        for (int y = startY; y < endY; ++y) {
            auto* row = dst.ptr<cv::Vec4b>(y);
            const float py = y + 0.5f;
            for (int x = startX; x < endX; ++x) {
                const float px = x + 0.5f;
                const float d = std::hypot(px - centerX, py - centerY);
                if (d > r + 0.5f) {
                    row[x][3] = 0;
                } else if (d > r - 0.5f) {
                    const float coverage = std::clamp(r + 0.5f - d, 0.0f, 1.0f);
                    row[x][3] = static_cast<uint8_t>(row[x][3] * coverage);
                }
            }
        }
    };

    // Top-Left corner
    applyCorner(0, intR, 0, intR, r, r);
    // Top-Right corner
    applyCorner(w - intR, w, 0, intR, w - r, r);
    // Bottom-Left corner
    applyCorner(0, intR, h - intR, h, r, h - r);
    // Bottom-Right corner
    applyCorner(w - intR, w, h - intR, h, w - r, h - r);

    return dst;
}

bool initializeMarkupTextRenderer() {
    std::lock_guard lock(g_gdiPlusMutex);
    if (g_gdiPlusToken != 0) return true;

    Gdiplus::GdiplusStartupInput input;
    ULONG_PTR token = 0;
    const auto status = Gdiplus::GdiplusStartup(&token, &input, nullptr);
    if (status != Gdiplus::Ok) {
        LOG_ERROR("GDI+ 文本渲染器初始化失败, status={}", static_cast<int>(status));
        return false;
    }
    g_gdiPlusToken = token;
    LOG_DEBUG("GDI+ 文本渲染器已初始化");
    return true;
}

void shutdownMarkupTextRenderer() {
    ULONG_PTR token = 0;
    {
        std::lock_guard lock(g_gdiPlusMutex);
        token = g_gdiPlusToken;
        g_gdiPlusToken = 0;
    }
    if (token != 0) {
        // Deliberately before FreeLibrary and without the mutex: GDI+ waits for
        // its helper thread during shutdown.
        Gdiplus::GdiplusShutdown(token);
        LOG_DEBUG("GDI+ 文本渲染器已关闭");
    }
}

static std::atomic<uint32_t> g_elementIdCounter{1};

// ─────────────────────────────────────────────────────────────────────────────
// MarkupElement 矢量操作
// ─────────────────────────────────────────────────────────────────────────────

cv::Rect MarkupElement::getBoundingBox() const {
    if (const auto* handler = MarkupToolRegistry::instance().getHandler(tool)) {
        return handler->getBoundingBox(*this);
    }
    return cv::Rect();
}

HitArea MarkupElement::hitTestEx(cv::Point pt, int padding) const {
    if (const auto* handler = MarkupToolRegistry::instance().getHandler(tool)) {
        return handler->hitTest(*this, pt, padding);
    }
    return HitArea::None;
}

void MarkupElement::moveBy(int dx, int dy) {
    startPt.x += dx; startPt.y += dy;
    endPt.x += dx; endPt.y += dy;
    for (auto& pt : penPoints) {
        pt.x += dx; pt.y += dy;
    }
}

void MarkupElement::resize(int dx, int dy, HitArea handle) {
    if (const auto* handler = MarkupToolRegistry::instance().getHandler(tool)) {
        handler->resize(*this, dx, dy, handle);
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// 底图管理
// ─────────────────────────────────────────────────────────────────────────────

int MarkupEngine::currentNumber(NumberFormat format) const noexcept {
    return m_nextNumbers[numberFormatSlot(format)];
}

void MarkupEngine::resetNumber(int start, NumberFormat format) noexcept {
    m_nextNumbers[numberFormatSlot(format)] = (std::max)(1, start);
}

void MarkupEngine::resetAllNumbers() noexcept {
    m_nextNumbers.fill(1);
}

void MarkupEngine::recalculateNextNumbers() noexcept {
    std::array<int, static_cast<size_t>(NumberFormat::COUNT)> maxByFormat{};
    for (const auto& elem : m_elements) {
        if (elem && elem->tool == MarkupTool::Number) {
            const size_t slot = numberFormatSlot(elem->numberFormat);
            maxByFormat[slot] = (std::max)(maxByFormat[slot], elem->numberValue);
        }
    }
    for (size_t i = 0; i < m_nextNumbers.size(); ++i) {
        m_nextNumbers[i] = maxByFormat[i] + 1;
    }
}

bool MarkupEngine::elementEquivalent(const MarkupElement& a, const MarkupElement& b) noexcept {
    if (a.id != b.id ||
        a.tool != b.tool ||
        a.color != b.color ||
        std::abs(a.thickness - b.thickness) > 1e-3f ||
        a.lineStyle != b.lineStyle ||
        a.fill != b.fill ||
        std::abs(a.cornerRadius - b.cornerRadius) > 1e-3f ||
        a.arrowStyle != b.arrowStyle ||
        a.smoothCurve != b.smoothCurve ||
        a.hasCustomCaps != b.hasCustomCaps ||
        a.startCap != b.startCap ||
        a.endCap != b.endCap ||
        a.startPt != b.startPt ||
        a.endPt != b.endPt ||
        a.text != b.text ||
        std::abs(a.fontSize - b.fontSize) > 1e-3f ||
        a.textOutline != b.textOutline ||
        a.textOutlineColor != b.textOutlineColor ||
        std::abs(a.outlineThickness - b.outlineThickness) > 1e-3f ||
        a.hasCustomOutline != b.hasCustomOutline ||
        a.numberValue != b.numberValue ||
        std::abs(a.dpiScale - b.dpiScale) > 1e-3f ||
        a.numberShape != b.numberShape ||
        a.numberFormat != b.numberFormat ||
        a.hasLeaderArrow != b.hasLeaderArrow ||
        a.mosaicBlockSize != b.mosaicBlockSize ||
        std::abs(a.magnifierScale - b.magnifierScale) > 1e-3f ||
        a.magnifierRadius != b.magnifierRadius ||
        a.magnifierRadiusY != b.magnifierRadiusY ||
        a.magnifierCalloutStyle != b.magnifierCalloutStyle ||
        std::abs(a.spotlightDimAlpha - b.spotlightDimAlpha) > 1e-3f ||
        a.spotlightEllipse != b.spotlightEllipse ||
        a.spotlightShowBorder != b.spotlightShowBorder ||
        a.watermarkText != b.watermarkText ||
        std::abs(a.watermarkOpacity - b.watermarkOpacity) > 1e-3f ||
        std::abs(a.watermarkAngle - b.watermarkAngle) > 1e-3f ||
        a.watermarkSpacing != b.watermarkSpacing ||
        a.inpaintRadius != b.inpaintRadius ||
        a.penPoints.size() != b.penPoints.size()) {
        return false;
    }
    for (size_t i = 0; i < a.penPoints.size(); ++i) {
        if (a.penPoints[i] != b.penPoints[i]) return false;
    }
    return true;
}

bool MarkupEngine::elementsEquivalent(const std::vector<MarkupElement>& a,
                                      const std::vector<MarkupElement>& b) noexcept {
    if (a.size() != b.size()) return false;
    for (size_t i = 0; i < a.size(); ++i) {
        if (!elementEquivalent(a[i], b[i])) return false;
    }
    return true;
}

MarkupEngine::MarkupSnapshot MarkupEngine::captureSnapshot() const {
    MarkupSnapshot snap;
    snap.elements.reserve(m_elements.size());
    for (const auto& elem : m_elements) {
        if (!elem) continue;
        MarkupElement copy = *elem;
        copy.isActive = false;
        copy.isEditing = false;
        snap.elements.push_back(std::move(copy));
    }
    snap.nextNumbers = m_nextNumbers;
    return snap;
}

void MarkupEngine::pushUndoSnapshot() {
    MarkupSnapshot cur = captureSnapshot();
    if (!m_undoHistory.empty() &&
        m_undoHistory.back().nextNumbers == cur.nextNumbers &&
        elementsEquivalent(m_undoHistory.back().elements, cur.elements)) {
        m_redoHistory.clear();
        return;
    }
    if (m_undoHistory.size() >= kMaxUndoSteps) {
        m_undoHistory.pop_front();
    }
    m_undoHistory.push_back(std::move(cur));
    m_redoHistory.clear();
}

void MarkupEngine::commitUndoSnapshot(MarkupSnapshot&& beforeState) {
    const MarkupSnapshot cur = captureSnapshot();
    if (beforeState.nextNumbers == cur.nextNumbers &&
        elementsEquivalent(beforeState.elements, cur.elements)) {
        return;
    }
    if (m_undoHistory.size() >= kMaxUndoSteps) {
        m_undoHistory.pop_front();
    }
    m_undoHistory.push_back(std::move(beforeState));
    m_redoHistory.clear();
}

void MarkupEngine::restoreSnapshot(const MarkupSnapshot& snapshot) {
    m_elements.clear();
    m_elements.reserve(snapshot.elements.size());
    uint32_t maxId = 0;
    for (const auto& item : snapshot.elements) {
        auto copy = std::make_unique<MarkupElement>(item);
        copy->isActive = false;
        copy->isEditing = false;
        if (copy->id == 0) {
            copy->id = g_elementIdCounter++;
        }
        maxId = (std::max)(maxId, copy->id);
        m_elements.push_back(std::move(copy));
    }
    uint32_t curCounter = g_elementIdCounter.load(std::memory_order_relaxed);
    while (curCounter <= maxId &&
           !g_elementIdCounter.compare_exchange_weak(curCounter, maxId + 1, std::memory_order_relaxed)) {}

    m_nextNumbers = snapshot.nextNumbers;
    m_underlayCache.release();
    m_underlayElements.clear();
}

std::vector<MarkupElement> MarkupEngine::exportElements() const {
    return captureSnapshot().elements;
}

void MarkupEngine::importElements(const std::vector<MarkupElement>& elems) {
    m_undoHistory.clear();
    m_redoHistory.clear();
    m_elements.clear();
    m_elements.reserve(elems.size());

    uint32_t maxId = 0;
    for (const auto& src : elems) {
        auto copy = std::make_unique<MarkupElement>(src);
        copy->isActive = false;
        copy->isEditing = false;
        if (copy->id == 0) {
            copy->id = g_elementIdCounter++;
        }
        maxId = (std::max)(maxId, copy->id);
        m_elements.push_back(std::move(copy));
    }
    uint32_t curCounter = g_elementIdCounter.load(std::memory_order_relaxed);
    while (curCounter <= maxId &&
           !g_elementIdCounter.compare_exchange_weak(curCounter, maxId + 1, std::memory_order_relaxed)) {}

    recalculateNextNumbers();
    m_underlayCache.release();
    m_underlayElements.clear();
}

void MarkupEngine::setBaseImage(const cv::Mat& image) {
    m_baseImage = image.clone();
    m_elements.clear();
    m_undoHistory.clear();
    m_redoHistory.clear();
    m_filterRoiCache.clear();
    m_underlayCache.release();
    m_underlayElements.clear();
    ++m_baseRevision;
    resetAllNumbers();
    LOG_DEBUG("标注引擎: 设置底图 {}x{}", image.cols, image.rows);
}

void MarkupEngine::updateBaseImage(const cv::Mat& image) {
    // 仅替换底图，保留标注/撤销栈/序号状态（选区二次调整后重裁用）
    m_baseImage = image.clone();
    m_filterRoiCache.clear();
    m_underlayCache.release();
    m_underlayElements.clear();
    ++m_baseRevision;
}

void MarkupEngine::translateAll(int dx, int dy) {
    if (dx == 0 && dy == 0) return;
    for (auto& e : m_elements) {
        if (e) e->moveBy(dx, dy);
    }
    for (auto& snap : m_undoHistory) {
        for (auto& e : snap.elements) e.moveBy(dx, dy);
    }
    for (auto& snap : m_redoHistory) {
        for (auto& e : snap.elements) e.moveBy(dx, dy);
    }
    m_filterRoiCache.clear();
    m_underlayCache.release();
    m_underlayElements.clear();
}

cv::Mat MarkupEngine::getCompositeImage(bool includeActiveHandles) const {
    if (m_baseImage.empty()) return {};
    if (m_elements.empty()) return m_baseImage.clone();

    auto isStageCForeground = [](MarkupTool t) noexcept {
        return t != MarkupTool::Mosaic &&
               t != MarkupTool::Blur &&
               t != MarkupTool::Inpaint &&
               t != MarkupTool::Spotlight;
    };

    const size_t total = m_elements.size();
    if (total >= 2 && m_elements.back() && isStageCForeground(m_elements.back()->tool)) {
        const size_t prefixCount = total - 1;
        bool cacheHit = (!m_underlayCache.empty() &&
                         m_underlayBaseRev == m_baseRevision &&
                         m_underlayCache.size() == m_baseImage.size() &&
                         m_underlayCache.type() == m_baseImage.type() &&
                         m_underlayElements.size() == prefixCount);
        if (cacheHit) {
            for (size_t i = 0; i < prefixCount; ++i) {
                if (!m_elements[i] || !elementEquivalent(*m_elements[i], m_underlayElements[i])) {
                    cacheHit = false;
                    break;
                }
            }
        }

        if (!cacheHit) {
            cv::Mat prefixCanvas = m_baseImage.clone();
            renderSubset(prefixCanvas, prefixCount, false);
            m_underlayCache = prefixCanvas;
            m_underlayBaseRev = m_baseRevision;
            m_underlayElements.clear();
            m_underlayElements.reserve(prefixCount);
            for (size_t i = 0; i < prefixCount; ++i) {
                if (m_elements[i]) {
                    MarkupElement spec = *m_elements[i];
                    spec.isActive = false;
                    spec.isEditing = false;
                    m_underlayElements.push_back(std::move(spec));
                }
            }
        }

        cv::Mat result = m_underlayCache.clone();
        renderElement(result, *m_elements.back(), false);
        if (includeActiveHandles) {
            for (const auto& elem : m_elements) {
                if (elem && elem->isActive) {
                    if (const auto* handler = MarkupToolRegistry::instance().getHandler(elem->tool)) {
                        handler->renderActiveHandles(result, *elem);
                    }
                }
            }
        }
        return result;
    }

    cv::Mat result = m_baseImage.clone();
    renderAll(result, includeActiveHandles);
    return result;
}

// ─────────────────────────────────────────────────────────────────────────────
// 元素操作
// ─────────────────────────────────────────────────────────────────────────────

MarkupElement* MarkupEngine::addElement(std::unique_ptr<MarkupElement> element) {
    if (!element) return nullptr;
    pushUndoSnapshot();
    if (element->id == 0) {
        element->id = g_elementIdCounter++;
    }
    m_elements.push_back(std::move(element));
    return m_elements.back().get();
}

bool MarkupEngine::undo() {
    if (m_undoHistory.empty()) return false;

    MarkupSnapshot current = captureSnapshot();
    if (m_redoHistory.size() >= kMaxUndoSteps) {
        m_redoHistory.pop_front();
    }
    m_redoHistory.push_back(std::move(current));

    MarkupSnapshot prev = std::move(m_undoHistory.back());
    m_undoHistory.pop_back();
    restoreSnapshot(prev);

    LOG_DEBUG("标注引擎: 撤销, 剩余元素数={}", m_elements.size());
    return true;
}

bool MarkupEngine::redo() {
    if (m_redoHistory.empty()) return false;

    MarkupSnapshot current = captureSnapshot();
    if (m_undoHistory.size() >= kMaxUndoSteps) {
        m_undoHistory.pop_front();
    }
    m_undoHistory.push_back(std::move(current));

    MarkupSnapshot next = std::move(m_redoHistory.back());
    m_redoHistory.pop_back();
    restoreSnapshot(next);

    LOG_DEBUG("标注引擎: 重做, 剩余元素数={}", m_elements.size());
    return true;
}

void MarkupEngine::clearAll() {
    m_elements.clear();
    m_undoHistory.clear();
    m_redoHistory.clear();
    m_filterRoiCache.clear();
    m_underlayCache.release();
    m_underlayElements.clear();
    resetAllNumbers();
}

void MarkupEngine::resetSession() {
    clearAll();
    m_baseImage.release();
    m_underlayCache.release();
    m_filterRoiCache.clear();
    m_elements.shrink_to_fit();
    m_underlayElements.shrink_to_fit();
    ++m_baseRevision;
}

bool MarkupEngine::clearElementsUndoable() {
    if (m_elements.empty()) {
        resetAllNumbers();
        return false;
    }
    pushUndoSnapshot();
    m_elements.clear();
    m_underlayCache.release();
    m_underlayElements.clear();
    resetAllNumbers();
    return true;
}

void MarkupEngine::removeElement(uint32_t id) {
    const auto found = std::find_if(m_elements.begin(), m_elements.end(),
        [id](const std::unique_ptr<MarkupElement>& e) { return e && e->id == id; });
    if (found == m_elements.end()) return;

    pushUndoSnapshot();
    m_elements.erase(std::remove_if(m_elements.begin(), m_elements.end(),
        [id](const std::unique_ptr<MarkupElement>& e) { return e && e->id == id; }),
        m_elements.end());
    m_filterRoiCache.erase(id);
    m_underlayCache.release();
    m_underlayElements.clear();

    recalculateNextNumbers();
}

HitResult MarkupEngine::getElementAtEx(cv::Point pt, int padding) const {
    // 倒序查找，优先命中上层元素
    for (auto it = m_elements.rbegin(); it != m_elements.rend(); ++it) {
        HitArea area = (*it)->hitTestEx(pt, padding);
        if (area != HitArea::None) {
            return { it->get(), area };
        }
    }
    return { nullptr, HitArea::None };
}

MarkupElement* MarkupEngine::getElementById(uint32_t id) const {
    for (const auto& elem : m_elements) {
        if (elem->id == id) return elem.get();
    }
    return nullptr;
}

// ─────────────────────────────────────────────────────────────────────────────
// 工具快捷方法
// ─────────────────────────────────────────────────────────────────────────────

MarkupElement* MarkupEngine::drawRectangle(cv::Point p1, cv::Point p2, MarkupColor color, float thickness) {
    auto elem = std::make_unique<MarkupElement>();
    elem->tool = MarkupTool::Rectangle;
    elem->startPt = cv::Point((std::min)(p1.x, p2.x), (std::min)(p1.y, p2.y));
    elem->endPt   = cv::Point((std::max)(p1.x, p2.x), (std::max)(p1.y, p2.y));
    elem->color = color;
    elem->thickness = thickness;
    return addElement(std::move(elem));
}

MarkupElement* MarkupEngine::drawLine(cv::Point p1, cv::Point p2, MarkupColor color, float thickness, LineStyle style) {
    auto elem = std::make_unique<MarkupElement>();
    elem->tool = MarkupTool::Line;
    elem->startPt = p1;
    elem->endPt = p2;
    elem->color = color;
    elem->thickness = thickness;
    elem->lineStyle = style;
    return addElement(std::move(elem));
}

MarkupElement* MarkupEngine::drawArrow(cv::Point from, cv::Point to, MarkupColor color, float thickness) {
    auto elem = std::make_unique<MarkupElement>();
    elem->tool = MarkupTool::Arrow;
    elem->startPt = from;
    elem->endPt = to;
    elem->color = color;
    elem->thickness = thickness;
    return addElement(std::move(elem));
}

MarkupElement* MarkupEngine::drawEllipse(cv::Point p1, cv::Point p2, MarkupColor color, float thickness) {
    auto elem = std::make_unique<MarkupElement>();
    elem->tool = MarkupTool::Ellipse;
    elem->startPt = cv::Point((std::min)(p1.x, p2.x), (std::min)(p1.y, p2.y));
    elem->endPt   = cv::Point((std::max)(p1.x, p2.x), (std::max)(p1.y, p2.y));
    elem->color = color;
    elem->thickness = thickness;
    return addElement(std::move(elem));
}

MarkupElement* MarkupEngine::drawPenStroke(const std::vector<cv::Point>& points, MarkupColor color, float thickness) {
    if (points.size() < 2) return nullptr;
    auto elem = std::make_unique<MarkupElement>();
    elem->tool = MarkupTool::Pen;
    if (points.size() >= 3) {
        elem->penPoints = generateSmoothSpline(points, 8);
    } else {
        elem->penPoints = points;
    }
    elem->color = color;
    elem->thickness = thickness;
    return addElement(std::move(elem));
}

MarkupElement* MarkupEngine::drawHighlight(cv::Point p1, cv::Point p2, MarkupColor color) {
    auto elem = std::make_unique<MarkupElement>();
    elem->tool = MarkupTool::Highlight;
    elem->startPt = cv::Point((std::min)(p1.x, p2.x), (std::min)(p1.y, p2.y));
    elem->endPt   = cv::Point((std::max)(p1.x, p2.x), (std::max)(p1.y, p2.y));
    elem->color = color;
    elem->color.a = 210;  // 正片叠底荧光笔默认高鲜明度
    return addElement(std::move(elem));
}

MarkupElement* MarkupEngine::applyMosaic(cv::Point p1, cv::Point p2, int blockSize) {
    blockSize = (std::max)(2, blockSize);
    auto elem = std::make_unique<MarkupElement>();
    elem->tool = MarkupTool::Mosaic;
    elem->startPt = cv::Point((std::min)(p1.x, p2.x), (std::min)(p1.y, p2.y));
    elem->endPt   = cv::Point((std::max)(p1.x, p2.x), (std::max)(p1.y, p2.y));
    elem->mosaicBlockSize = blockSize;
    elem->thickness = static_cast<float>((std::max)(1, blockSize / 2));
    return addElement(std::move(elem));
}

MarkupElement* MarkupEngine::applyBlur(cv::Point p1, cv::Point p2, int kernelSize) {
    auto elem = std::make_unique<MarkupElement>();
    elem->tool = MarkupTool::Blur;
    elem->startPt = cv::Point((std::min)(p1.x, p2.x), (std::min)(p1.y, p2.y));
    elem->endPt   = cv::Point((std::max)(p1.x, p2.x), (std::max)(p1.y, p2.y));
    elem->mosaicBlockSize = kernelSize;
    elem->thickness = static_cast<float>((std::max)(1, kernelSize / 3));
    return addElement(std::move(elem));
}

MarkupElement* MarkupEngine::addText(cv::Point position, const std::string& text, MarkupColor color, float fontSize,
                                     bool textOutline, MarkupColor outlineColor) {
    auto elem = std::make_unique<MarkupElement>();
    elem->tool = MarkupTool::Text;
    elem->startPt = position;
    elem->endPt = position;
    elem->text = text;
    elem->color = color;
    elem->fontSize = fontSize;
    elem->textOutline = textOutline;
    elem->textOutlineColor = outlineColor;
    return addElement(std::move(elem));
}

MarkupElement* MarkupEngine::drawNumberMark(cv::Point position, MarkupColor color, float dpiScale,
                                            NumberBadgeShape shape, NumberFormat format) {
    const size_t slot = numberFormatSlot(format);
    const int assigned = m_nextNumbers[slot];
    auto elem = std::make_unique<MarkupElement>();
    elem->tool = MarkupTool::Number;
    elem->startPt = position;
    elem->endPt = position;
    elem->color = color;
    elem->numberValue = assigned;
    elem->dpiScale = dpiScale;
    elem->numberShape = shape;
    elem->numberFormat = format;
    MarkupElement* added = addElement(std::move(elem));
    m_nextNumbers[slot] = assigned + 1;
    return added;
}

int MarkupEngine::addNumberMark(cv::Point position, MarkupColor color, float dpiScale,
                                NumberBadgeShape shape, NumberFormat format) {
    auto* elem = drawNumberMark(position, color, dpiScale, shape, format);
    return elem ? elem->numberValue : 0;
}

MarkupElement* MarkupEngine::addMagnifier(cv::Point center, float scale, int radius) {
    auto elem = std::make_unique<MarkupElement>();
    elem->tool = MarkupTool::Magnifier;
    elem->startPt = center;
    elem->endPt = center;
    elem->color = MarkupColor::White();
    elem->thickness = 3.0f;
    elem->fill = true; // 默认展示精致倍率微晶胶囊，可通过填充开关切换显隐
    elem->magnifierScale = std::clamp(scale, 1.1f, 8.0f);
    elem->magnifierRadius = std::clamp(radius, 20, 500);
    elem->magnifierRadiusY = elem->magnifierRadius;
    return addElement(std::move(elem));
}

MarkupElement* MarkupEngine::addSpotlight(cv::Point p1, cv::Point p2, MarkupColor color, float dimAlpha, bool ellipse) {
    auto elem = std::make_unique<MarkupElement>();
    elem->tool = MarkupTool::Spotlight;
    elem->startPt = cv::Point((std::min)(p1.x, p2.x), (std::min)(p1.y, p2.y));
    elem->endPt   = cv::Point((std::max)(p1.x, p2.x), (std::max)(p1.y, p2.y));
    elem->color = color;
    elem->spotlightDimAlpha = dimAlpha;
    elem->spotlightEllipse = ellipse;
    LOG_DEBUG("标注引擎: 添加聚光灯 ({},{})→({},{}) dimAlpha={:.2f} ellipse={}",
              p1.x, p1.y, p2.x, p2.y, dimAlpha, ellipse);
    return addElement(std::move(elem));
}

MarkupElement* MarkupEngine::addWatermark(cv::Point p1, cv::Point p2, const std::string& text, float opacity, float angle) {
    auto elem = std::make_unique<MarkupElement>();
    elem->tool = MarkupTool::Watermark;
    elem->startPt = cv::Point((std::min)(p1.x, p2.x), (std::min)(p1.y, p2.y));
    elem->endPt   = cv::Point((std::max)(p1.x, p2.x), (std::max)(p1.y, p2.y));
    elem->watermarkText = text;
    elem->watermarkOpacity = opacity;
    elem->watermarkAngle = angle;
    LOG_DEBUG("标注引擎: 添加水印 ({},{})→({},{}) text='{}' opacity={:.2f} angle={:.1f}",
              p1.x, p1.y, p2.x, p2.y, text, opacity, angle);
    return addElement(std::move(elem));
}

MarkupElement* MarkupEngine::applyInpaint(cv::Point p1, cv::Point p2, int radius) {
    radius = std::clamp(radius, 1, 32);
    auto elem = std::make_unique<MarkupElement>();
    elem->tool = MarkupTool::Inpaint;
    elem->startPt = cv::Point((std::min)(p1.x, p2.x), (std::min)(p1.y, p2.y));
    elem->endPt   = cv::Point((std::max)(p1.x, p2.x), (std::max)(p1.y, p2.y));
    elem->inpaintRadius = radius;
    elem->thickness = static_cast<float>(radius);
    LOG_DEBUG("标注引擎: 添加智能消除 ({},{})→({},{}) radius={}",
              p1.x, p1.y, p2.x, p2.y, radius);
    return addElement(std::move(elem));
}

// ─────────────────────────────────────────────────────────────────────────────
// 统一 8 方向控制手柄几何与通用包围盒处理引擎 (HandleGeometry & DefaultBoxHandler)
// ─────────────────────────────────────────────────────────────────────────────

std::array<HandleGeometry::HandlePoint, 8> HandleGeometry::getBoxHandles(const cv::Rect& bbox) {
    const int x = bbox.x;
    const int y = bbox.y;
    const int w = bbox.width;
    const int h = bbox.height;
    const int cx = x + w / 2;
    const int cy = y + h / 2;

    return {{
        { cv::Point(x, y),         HitArea::LT },
        { cv::Point(cx, y),        HitArea::T  },
        { cv::Point(x + w, y),     HitArea::RT },
        { cv::Point(x + w, cy),    HitArea::R  },
        { cv::Point(x + w, y + h), HitArea::RB },
        { cv::Point(cx, y + h),    HitArea::B  },
        { cv::Point(x, y + h),     HitArea::LB },
        { cv::Point(x, cy),        HitArea::L  }
    }};
}

HitArea HandleGeometry::hitTestHandles(const cv::Rect& bbox, cv::Point pt, int handleHalfSize) {
    auto handles = getBoxHandles(bbox);
    const int hw = (std::max)(1, handleHalfSize);

    // 优先检测 4 个角手柄
    static constexpr std::array<size_t, 4> kCorners = {0, 2, 4, 6};
    for (size_t idx : kCorners) {
        const auto& hp = handles[idx];
        cv::Rect hrect(hp.point.x - hw, hp.point.y - hw, hw * 2, hw * 2);
        if (hrect.contains(pt)) return hp.area;
    }

    // 其次检测 4 个边中点手柄
    static constexpr std::array<size_t, 4> kEdges = {1, 3, 5, 7};
    for (size_t idx : kEdges) {
        const auto& hp = handles[idx];
        cv::Rect hrect(hp.point.x - hw, hp.point.y - hw, hw * 2, hw * 2);
        if (hrect.contains(pt)) return hp.area;
    }

    return HitArea::None;
}

void HandleGeometry::renderBoxHandles(cv::Mat& canvas, const cv::Rect& bbox,
                                     const cv::Scalar& activeColor,
                                     int handleHalfSize) {
    cv::rectangle(canvas, bbox, activeColor, 1, cv::LINE_AA);

    const int hw = (std::max)(2, handleHalfSize);
    auto handles = getBoxHandles(bbox);

    for (const auto& hp : handles) {
        cv::Rect hrect(hp.point.x - hw, hp.point.y - hw, hw * 2, hw * 2);
        cv::rectangle(canvas, hrect, cv::Scalar(255, 255, 255), cv::FILLED, cv::LINE_AA);
        cv::rectangle(canvas, hrect, activeColor, 1, cv::LINE_AA);
    }
}

cv::Rect HandleGeometry::computeResizedRect(const cv::Rect& originalRect, int dx, int dy, HitArea handle,
                                           int minW, int minH) {
    int l = originalRect.x;
    int t = originalRect.y;
    int r = originalRect.x + originalRect.width;
    int b = originalRect.y + originalRect.height;

    switch (handle) {
        case HitArea::LT: l += dx; t += dy; break;
        case HitArea::T:  t += dy; break;
        case HitArea::RT: r += dx; t += dy; break;
        case HitArea::R:  r += dx; break;
        case HitArea::RB: r += dx; b += dy; break;
        case HitArea::B:  b += dy; break;
        case HitArea::LB: l += dx; b += dy; break;
        case HitArea::L:  l += dx; break;
        default: break;
    }

    if (r - l < minW) {
        if (handle == HitArea::L || handle == HitArea::LT || handle == HitArea::LB) {
            l = r - minW;
        } else {
            r = l + minW;
        }
    }
    if (b - t < minH) {
        if (handle == HitArea::T || handle == HitArea::LT || handle == HitArea::RT) {
            t = b - minH;
        } else {
            b = t + minH;
        }
    }

    return cv::Rect(l, t, r - l, b - t);
}

int HandleGeometry::computeScalarDelta(int dx, int dy, HitArea handle) {
    switch (handle) {
        case HitArea::R:  return dx;
        case HitArea::L:  return -dx;
        case HitArea::B:  return dy;
        case HitArea::T:  return -dy;
        case HitArea::RB: return static_cast<int>(std::round((dx + dy) * 0.5f));
        case HitArea::LT: return static_cast<int>(std::round((-dx - dy) * 0.5f));
        case HitArea::RT: return static_cast<int>(std::round((dx - dy) * 0.5f));
        case HitArea::LB: return static_cast<int>(std::round((-dx + dy) * 0.5f));
        default: return 0;
    }
}

cv::Point HandleGeometry::computeAnchoredOrigin(const cv::Point& currentOrigin, int dW, int dH, HitArea handle) {
    cv::Point origin = currentOrigin;
    switch (handle) {
        case HitArea::R:
            // 右边拉伸：左边固定，垂直中心绝对锚定
            origin.y -= dH / 2;
            break;
        case HitArea::L:
            // 左边拉伸：右边固定，垂直中心绝对锚定
            origin.x -= dW;
            origin.y -= dH / 2;
            break;
        case HitArea::B:
            // 下边拉伸：顶边固定，水平中心绝对锚定
            origin.x -= dW / 2;
            break;
        case HitArea::T:
            // 上边拉伸：底边固定，水平中心绝对锚定
            origin.x -= dW / 2;
            origin.y -= dH;
            break;
        case HitArea::RB:
            // 右下角拉伸：左上角固定
            break;
        case HitArea::LT:
            // 左上角拉伸：右下角固定
            origin.x -= dW;
            origin.y -= dH;
            break;
        case HitArea::RT:
            // 右上角拉伸：左下角固定
            origin.y -= dH;
            break;
        case HitArea::LB:
            // 左下角拉伸：右上角固定
            origin.x -= dW;
            break;
        default:
            break;
    }
    return origin;
}

cv::Rect DefaultBoxHandler::getBoundingBox(const MarkupElement& element) const {
    int x1 = (std::min)(element.startPt.x, element.endPt.x);
    int y1 = (std::min)(element.startPt.y, element.endPt.y);
    int x2 = (std::max)(element.startPt.x, element.endPt.x);
    int y2 = (std::max)(element.startPt.y, element.endPt.y);
    return cv::Rect(x1, y1, x2 - x1, y2 - y1);
}

HitArea DefaultBoxHandler::hitTestHandles(const MarkupElement& element, cv::Point pt, int padding) const {
    if (!element.isActive) return HitArea::None;
    cv::Rect bbox = getBoundingBox(element);
    int hw = (std::max)(padding, HandleGeometry::kDefaultHandleHalfSize);
    return HandleGeometry::hitTestHandles(bbox, pt, hw);
}

HitArea DefaultBoxHandler::hitTest(const MarkupElement& element, cv::Point pt, int padding) const {
    cv::Rect bbox = getBoundingBox(element);
    if (element.isActive) {
        HitArea h = hitTestHandles(element, pt, padding);
        if (h != HitArea::None) return h;
    }
    int p = (std::max)(padding, 6);
    cv::Rect expandedBox(bbox.x - p, bbox.y - p, bbox.width + p * 2, bbox.height + p * 2);
    if (expandedBox.contains(pt)) return HitArea::Body;
    return HitArea::None;
}

void DefaultBoxHandler::resize(MarkupElement& element, int dx, int dy, HitArea handle) const {
    cv::Rect origBox = getBoundingBox(element);
    cv::Rect newBox = HandleGeometry::computeResizedRect(origBox, dx, dy, handle,
                                                        HandleGeometry::kDefaultMinBoxSize,
                                                        HandleGeometry::kDefaultMinBoxSize);
    element.startPt = cv::Point(newBox.x, newBox.y);
    element.endPt = cv::Point(newBox.x + newBox.width, newBox.y + newBox.height);
    LOG_DEBUG("MarkupEngine: DefaultBoxHandler::resize tool={}, handle={}, dx={}, dy={}, bounds=[{},{},{},{}]",
              static_cast<int>(element.tool), static_cast<int>(handle), dx, dy,
              newBox.x, newBox.y, newBox.x + newBox.width, newBox.y + newBox.height);
}

void DefaultBoxHandler::renderActiveHandles(cv::Mat& canvas, const MarkupElement& element) const {
    cv::Rect bbox = getBoundingBox(element);
    HandleGeometry::renderBoxHandles(canvas, bbox);
}

// ─────────────────────────────────────────────────────────────────────────────
// 渲染
// ─────────────────────────────────────────────────────────────────────────────

namespace {

void drawPatternedPolyline(cv::Mat& canvas, const std::vector<cv::Point>& points, bool closed, const cv::Scalar& color, int thick, LineStyle style) {
    if (points.size() < 2) return;
    if (style == LineStyle::Solid) {
        cv::polylines(canvas, points, closed, color, thick, cv::LINE_AA);
        return;
    }

    const float t = static_cast<float>((std::max)(1, thick));
    std::vector<float> pattern;
    if (style == LineStyle::Dashed) {
        pattern = { (std::max)(10.0f, 2.2f * t), (std::max)(6.0f, 1.8f * t) + t };
    } else if (style == LineStyle::Dotted) {
        pattern = { (std::max)(1.0f, 0.20f * t), (std::max)(4.5f, 1.4f * t) + t };
    } else if (style == LineStyle::DashDot) {
        pattern = { (std::max)(10.0f, 2.2f * t), (std::max)(5.0f, 1.6f * t) + t,
                    (std::max)(1.0f, 0.20f * t), (std::max)(5.0f, 1.6f * t) + t };
    } else {
        pattern = { 1000.0f, 0.0f };
    }

    size_t patIdx = 0;
    float patRem = pattern[0];
    bool isDraw = true;
    std::vector<cv::Point> dashPts;

    auto flushDash = [&]() {
        if (dashPts.empty()) return;
        if (dashPts.size() == 1) {
            cv::line(canvas, dashPts[0], dashPts[0], color, thick, cv::LINE_AA);
        } else if (dashPts.size() == 2) {
            cv::line(canvas, dashPts[0], dashPts[1], color, thick, cv::LINE_AA);
        } else {
            cv::polylines(canvas, dashPts, false, color, thick, cv::LINE_AA);
        }
        dashPts.clear();
    };

    size_t numEdges = closed ? points.size() : points.size() - 1;
    bool anyEdgeDrawn = false;
    for (size_t i = 0; i < numEdges; ++i) {
        cv::Point ptA = points[i];
        cv::Point ptB = points[(i + 1) % points.size()];

        float dx = static_cast<float>(ptB.x - ptA.x);
        float dy = static_cast<float>(ptB.y - ptA.y);
        float dist = std::hypot(dx, dy);
        if (dist < 1e-4f) continue;
        anyEdgeDrawn = true;

        float ux = dx / dist;
        float uy = dy / dist;
        float edgeTraveled = 0.0f;

        while (edgeTraveled < dist) {
            float canAdvance = (std::min)(dist - edgeTraveled, patRem);
            float nextTraveled = edgeTraveled + canAdvance;

            if (isDraw) {
                cv::Point pStart(static_cast<int>(std::round(ptA.x + ux * edgeTraveled)),
                                 static_cast<int>(std::round(ptA.y + uy * edgeTraveled)));
                cv::Point pEnd(static_cast<int>(std::round(ptA.x + ux * nextTraveled)),
                               static_cast<int>(std::round(ptA.y + uy * nextTraveled)));
                if (dashPts.empty() || dashPts.back() != pStart) {
                    dashPts.push_back(pStart);
                }
                if (dashPts.back() != pEnd) {
                    dashPts.push_back(pEnd);
                }
            }

            edgeTraveled = nextTraveled;
            patRem -= canAdvance;
            if (patRem <= 1e-4f) {
                if (isDraw) {
                    flushDash();
                }
                patIdx = (patIdx + 1) % pattern.size();
                patRem = pattern[patIdx];
                isDraw = (patIdx % 2 == 0);
            }
        }
    }
    if (!anyEdgeDrawn && !points.empty()) {
        cv::line(canvas, points[0], points[0], color, thick, cv::LINE_AA);
        return;
    }
    if (isDraw) {
        flushDash();
    }
}

void drawPatternedLine(cv::Mat& canvas, cv::Point p1, cv::Point p2, const cv::Scalar& color, int thick, LineStyle style) {
    if (style == LineStyle::Solid) {
        cv::line(canvas, p1, p2, color, thick, cv::LINE_AA);
    } else {
        drawPatternedPolyline(canvas, { p1, p2 }, false, color, thick, style);
    }
}

// 通用 10 种端点绘制与主线回缩截断计算 (PixPin 标杆：空心端点自动截断主线不穿心)
void drawEndpointCap(cv::Mat& canvas, cv::Point tip, double tangentAngle, LineCapStyle capStyle,
                     const cv::Scalar& color, int thick, double maxHeadLen, float& outShaftTrimLength) {
    outShaftTrimLength = 0.0f;
    if (capStyle == LineCapStyle::None) return;

    const double cA = std::cos(tangentAngle);
    const double sA = std::sin(tangentAngle);
    const double nX = -sA;
    const double nY = cA;
    double headLen = (std::max)(12.0, static_cast<double>(thick) * 3.5);
    if (maxHeadLen > 4.0 && headLen > maxHeadLen) {
        headLen = maxHeadLen;
    }
    const int outlineThick = (std::max)(1, static_cast<int>(std::round(static_cast<double>(thick) * 0.85)));

    switch (capStyle) {
        case LineCapStyle::OpenArrow: {
            const double wingAngle = CV_PI / 6.0;
            cv::Point w1(static_cast<int>(std::round(tip.x - headLen * std::cos(tangentAngle - wingAngle))),
                         static_cast<int>(std::round(tip.y - headLen * std::sin(tangentAngle - wingAngle))));
            cv::Point w2(static_cast<int>(std::round(tip.x - headLen * std::cos(tangentAngle + wingAngle))),
                         static_cast<int>(std::round(tip.y - headLen * std::sin(tangentAngle + wingAngle))));
            cv::line(canvas, tip, w1, color, thick, cv::LINE_AA);
            cv::line(canvas, tip, w2, color, thick, cv::LINE_AA);
            outShaftTrimLength = 0.0f;
            break;
        }
        case LineCapStyle::SolidTriangle: {
            const double halfW = headLen * 0.45;
            cv::Point basePt(static_cast<int>(std::round(tip.x - headLen * cA)),
                             static_cast<int>(std::round(tip.y - headLen * sA)));
            cv::Point w1(static_cast<int>(std::round(basePt.x + halfW * nX)),
                         static_cast<int>(std::round(basePt.y + halfW * nY)));
            cv::Point w2(static_cast<int>(std::round(basePt.x - halfW * nX)),
                         static_cast<int>(std::round(basePt.y - halfW * nY)));
            std::vector<std::vector<cv::Point>> headPoly = { { tip, w1, w2 } };
            cv::fillPoly(canvas, headPoly, color, cv::LINE_AA);
            outShaftTrimLength = static_cast<float>(headLen);
            break;
        }
        case LineCapStyle::HollowTriangle: {
            double triLen = (std::max)(15.0, static_cast<double>(thick) * 3.8);
            if (maxHeadLen > 4.0 && triLen > maxHeadLen) triLen = maxHeadLen;
            const double halfW = triLen * 0.54;
            cv::Point basePt(static_cast<int>(std::round(tip.x - triLen * cA)),
                             static_cast<int>(std::round(tip.y - triLen * sA)));
            cv::Point w1(static_cast<int>(std::round(basePt.x + halfW * nX)),
                         static_cast<int>(std::round(basePt.y + halfW * nY)));
            cv::Point w2(static_cast<int>(std::round(basePt.x - halfW * nX)),
                         static_cast<int>(std::round(basePt.y - halfW * nY)));
            std::vector<cv::Point> tri = { tip, w1, w2 };
            cv::polylines(canvas, tri, true, color, outlineThick, cv::LINE_AA);
            outShaftTrimLength = static_cast<float>(triLen + static_cast<double>(thick) * 0.45);
            break;
        }
        case LineCapStyle::StealthArrow: {
            const double halfW = headLen * 0.55;
            cv::Point w1(static_cast<int>(std::round(tip.x - headLen * cA + halfW * nX)),
                         static_cast<int>(std::round(tip.y - headLen * sA + halfW * nY)));
            cv::Point w2(static_cast<int>(std::round(tip.x - headLen * cA - halfW * nX)),
                         static_cast<int>(std::round(tip.y - headLen * sA - halfW * nY)));
            cv::Point notch(static_cast<int>(std::round(tip.x - headLen * 0.65 * cA)),
                            static_cast<int>(std::round(tip.y - headLen * 0.65 * sA)));
            std::vector<std::vector<cv::Point>> headPoly = { { tip, w1, notch, w2 } };
            cv::fillPoly(canvas, headPoly, color, cv::LINE_AA);
            outShaftTrimLength = static_cast<float>(headLen * 0.65);
            break;
        }
        case LineCapStyle::SolidCircle: {
            double radius = (std::max)(4.5, static_cast<double>(thick) * 1.45);
            if (maxHeadLen > 4.0 && radius * 1.6 > maxHeadLen) {
                radius = (std::max)(2.5, maxHeadLen / 1.6);
            }
            cv::Point center(static_cast<int>(std::round(tip.x - radius * 0.6 * cA)),
                             static_cast<int>(std::round(tip.y - radius * 0.6 * sA)));
            cv::circle(canvas, center, static_cast<int>(std::round(radius)), color, cv::FILLED, cv::LINE_AA);
            outShaftTrimLength = static_cast<float>(radius * 0.6);
            break;
        }
        case LineCapStyle::HollowCircle: {
            double radius = (std::max)(5.5, static_cast<double>(thick) * 1.65);
            if (maxHeadLen > 4.0 && radius * 2.0 > maxHeadLen) {
                radius = (std::max)(2.5, maxHeadLen * 0.5);
            }
            cv::Point center(static_cast<int>(std::round(tip.x - radius * cA)),
                             static_cast<int>(std::round(tip.y - radius * sA)));
            cv::circle(canvas, center, static_cast<int>(std::round(radius)), color, outlineThick, cv::LINE_AA);
            outShaftTrimLength = static_cast<float>(radius * 2.0 + static_cast<double>(thick) * 0.35);
            break;
        }
        case LineCapStyle::SolidDiamond: {
            const double dLen = headLen * 0.96;
            const double halfW = dLen * 0.48;
            cv::Point backPt(static_cast<int>(std::round(tip.x - dLen * cA)),
                             static_cast<int>(std::round(tip.y - dLen * sA)));
            cv::Point midPt(static_cast<int>(std::round(tip.x - dLen * 0.5 * cA)),
                            static_cast<int>(std::round(tip.y - dLen * 0.5 * sA)));
            cv::Point w1(static_cast<int>(std::round(midPt.x + halfW * nX)),
                         static_cast<int>(std::round(midPt.y + halfW * nY)));
            cv::Point w2(static_cast<int>(std::round(midPt.x - halfW * nX)),
                         static_cast<int>(std::round(midPt.y - halfW * nY)));
            std::vector<std::vector<cv::Point>> poly = { { tip, w1, backPt, w2 } };
            cv::fillPoly(canvas, poly, color, cv::LINE_AA);
            outShaftTrimLength = static_cast<float>(dLen * 0.85);
            break;
        }
        case LineCapStyle::HollowDiamond: {
            double dLen = headLen * 1.05;
            if (maxHeadLen > 4.0 && dLen > maxHeadLen) {
                dLen = maxHeadLen;
            }
            const double halfW = dLen * 0.50;
            cv::Point backPt(static_cast<int>(std::round(tip.x - dLen * cA)),
                             static_cast<int>(std::round(tip.y - dLen * sA)));
            cv::Point midPt(static_cast<int>(std::round(tip.x - dLen * 0.5 * cA)),
                            static_cast<int>(std::round(tip.y - dLen * 0.5 * sA)));
            cv::Point w1(static_cast<int>(std::round(midPt.x + halfW * nX)),
                         static_cast<int>(std::round(midPt.y + halfW * nY)));
            cv::Point w2(static_cast<int>(std::round(midPt.x - halfW * nX)),
                         static_cast<int>(std::round(midPt.y - halfW * nY)));
            std::vector<cv::Point> poly = { tip, w1, backPt, w2 };
            cv::polylines(canvas, poly, true, color, outlineThick, cv::LINE_AA);
            outShaftTrimLength = static_cast<float>(dLen + static_cast<double>(thick) * 0.35);
            break;
        }
        case LineCapStyle::BarStop: {
            const double halfW = (std::max)(6.0, static_cast<double>(thick) * 2.1);
            cv::Point w1(static_cast<int>(std::round(tip.x + halfW * nX)),
                         static_cast<int>(std::round(tip.y + halfW * nY)));
            cv::Point w2(static_cast<int>(std::round(tip.x - halfW * nX)),
                         static_cast<int>(std::round(tip.y - halfW * nY)));
            cv::line(canvas, w1, w2, color, thick, cv::LINE_AA);
            outShaftTrimLength = 0.0f;
            break;
        }
        default:
            break;
    }
}

std::vector<cv::Point> sampleQuadraticBezier(cv::Point p0, cv::Point p1, cv::Point p2, int steps = 28) {
    std::vector<cv::Point> out;
    out.reserve(static_cast<size_t>(steps) + 1);
    for (int i = 0; i <= steps; ++i) {
        const float t = static_cast<float>(i) / static_cast<float>(steps);
        const float u = 1.0f - t;
        const float x = u * u * static_cast<float>(p0.x) +
                        2.0f * u * t * static_cast<float>(p1.x) +
                        t * t * static_cast<float>(p2.x);
        const float y = u * u * static_cast<float>(p0.y) +
                        2.0f * u * t * static_cast<float>(p1.y) +
                        t * t * static_cast<float>(p2.y);
        cv::Point pt(static_cast<int>(std::lround(x)), static_cast<int>(std::lround(y)));
        if (out.empty() || out.back() != pt) {
            out.push_back(pt);
        }
    }
    if (out.size() == 1) {
        out.push_back(p2);
    }
    return out;
}

cv::Point computeDefaultArchMidPoint(cv::Point p1, cv::Point p2) {
    const float dx = static_cast<float>(p2.x - p1.x);
    const float dy = static_cast<float>(p2.y - p1.y);
    const float dist = std::hypot(dx, dy);
    if (dist < 6.0f) {
        return cv::Point((p1.x + p2.x) / 2, (p1.y + p2.y) / 2);
    }
    // 确保无论从左往右还是从右往左画，默认单拱弧线始终自然向上拱起 (ny <= 0)
    float nx = -dy / dist;
    float ny = dx / dist;
    if (ny > 0.0f) {
        nx = -nx;
        ny = -ny;
    }
    const float arch = std::clamp(dist * 0.18f, 8.0f, 48.0f);
    return cv::Point(
        static_cast<int>(std::lround((p1.x + p2.x) * 0.5f + nx * arch)),
        static_cast<int>(std::lround((p1.y + p2.y) * 0.5f + ny * arch)));
}

cv::Point getArrowOrLineCurveHandle(const MarkupElement& element) {
    if (element.penPoints.size() == 3) {
        // penPoints[1] 存储二次贝塞尔控制点 C，曲线在 t=0.5 处的真实顶点 M = 0.25*P0 + 0.5*C + 0.25*P2
        const cv::Point p0 = element.startPt;
        const cv::Point c = element.penPoints[1];
        const cv::Point p2 = element.endPt;
        return cv::Point(
            static_cast<int>(std::lround(0.25f * p0.x + 0.5f * c.x + 0.25f * p2.x)),
            static_cast<int>(std::lround(0.25f * p0.y + 0.5f * c.y + 0.25f * p2.y)));
    }
    if (element.smoothCurve) {
        return computeDefaultArchMidPoint(element.startPt, element.endPt);
    }
    return cv::Point((element.startPt.x + element.endPt.x) / 2,
                     (element.startPt.y + element.endPt.y) / 2);
}

std::vector<cv::Point> buildLineOrCurvePath(const MarkupElement& element) {
    if (element.penPoints.size() == 3 &&
        (element.tool == MarkupTool::Arrow || element.tool == MarkupTool::Line)) {
        if (!element.smoothCurve) {
            return { element.startPt, element.endPt };
        }
        return sampleQuadraticBezier(element.startPt, element.penPoints[1], element.endPt, 28);
    }
    if (element.penPoints.size() >= 3) {
        return element.smoothCurve ? generateSmoothSpline(element.penPoints, 10) : element.penPoints;
    }
    if (element.penPoints.size() == 2) {
        if (!element.smoothCurve) return element.penPoints;
    }
    const cv::Point p1 = (element.penPoints.size() == 2) ? element.penPoints[0] : element.startPt;
    const cv::Point p2 = (element.penPoints.size() == 2) ? element.penPoints[1] : element.endPt;
    if (!element.smoothCurve) {
        return { p1, p2 };
    }
    const float dx = static_cast<float>(p2.x - p1.x);
    const float dy = static_cast<float>(p2.y - p1.y);
    const float dist = std::hypot(dx, dy);
    if (dist < 6.0f) {
        return { p1, p2 };
    }
    // 箭头工具 (MarkupTool::Arrow) 开启平滑曲线时：采用优雅的单拱 C 形圆弧（杜绝怪异 S 形扭曲）
    if (element.tool == MarkupTool::Arrow) {
        const cv::Point midArch = computeDefaultArchMidPoint(p1, p2);
        const cv::Point bezierCtrl(
            static_cast<int>(std::lround(2.0f * midArch.x - 0.5f * (p1.x + p2.x))),
            static_cast<int>(std::lround(2.0f * midArch.y - 0.5f * (p1.y + p2.y))));
        return sampleQuadraticBezier(p1, bezierCtrl, p2, 28);
    }
    const float nx = -dy / dist;
    const float ny = dx / dist;
    const float arch = std::clamp(dist * 0.14f, 4.0f, 32.0f);
    cv::Point c1(static_cast<int>(std::round(p1.x + dx * 0.33f + nx * arch)),
                 static_cast<int>(std::round(p1.y + dy * 0.33f + ny * arch)));
    cv::Point c2(static_cast<int>(std::round(p1.x + dx * 0.67f - nx * arch)),
                 static_cast<int>(std::round(p1.y + dy * 0.67f - ny * arch)));
    return generateSmoothSpline({ p1, c1, c2, p2 }, 10);
}

std::vector<cv::Point> trimPolylineEnds(const std::vector<cv::Point>& pts, float startTrim, float endTrim) {
    if (pts.size() < 2) return pts;
    std::vector<float> cumLen(pts.size(), 0.0f);
    for (size_t i = 1; i < pts.size(); ++i) {
        cumLen[i] = cumLen[i - 1] + static_cast<float>(std::hypot(pts[i].x - pts[i - 1].x, pts[i].y - pts[i - 1].y));
    }
    const float totalLen = cumLen.back();
    if (totalLen <= 1.0f || startTrim + endTrim >= totalLen - 1.0f) {
        return {};
    }

    const float tStart = std::clamp(startTrim, 0.0f, totalLen);
    const float tEnd = std::clamp(totalLen - endTrim, tStart, totalLen);

    auto sampleAt = [&](float targetDist) -> cv::Point {
        for (size_t i = 1; i < pts.size(); ++i) {
            if (cumLen[i] >= targetDist || i == pts.size() - 1) {
                const float segLen = cumLen[i] - cumLen[i - 1];
                const float ratio = (segLen > 1e-4f) ? std::clamp((targetDist - cumLen[i - 1]) / segLen, 0.0f, 1.0f) : 0.0f;
                return cv::Point(
                    static_cast<int>(std::round(pts[i - 1].x + (pts[i].x - pts[i - 1].x) * ratio)),
                    static_cast<int>(std::round(pts[i - 1].y + (pts[i].y - pts[i - 1].y) * ratio)));
            }
        }
        return pts.back();
    };

    std::vector<cv::Point> out;
    out.push_back(sampleAt(tStart));
    for (size_t i = 1; i + 1 < pts.size(); ++i) {
        if (cumLen[i] > tStart + 0.5f && cumLen[i] < tEnd - 0.5f) {
            out.push_back(pts[i]);
        }
    }
    out.push_back(sampleAt(tEnd));
    return out;
}

void renderLineWithEndpointCaps(cv::Mat& canvas, const MarkupElement& element,
                                LineCapStyle startCap, LineCapStyle endCap) {
    auto pts = buildLineOrCurvePath(element);
    if (pts.size() < 2) return;

    auto color = element.color.toCvScalar();
    int thick = (std::max)(1, static_cast<int>(element.thickness));

    double totalLen = 0.0;
    for (size_t i = 1; i < pts.size(); ++i) {
        totalLen += std::hypot(pts[i].x - pts[i - 1].x, pts[i].y - pts[i - 1].y);
    }
    if (totalLen < 2.0) {
        if (startCap == LineCapStyle::None && endCap == LineCapStyle::None) {
            drawPatternedPolyline(canvas, pts, false, color, thick, element.lineStyle);
        }
        return;
    }

    const bool dualCaps = (startCap != LineCapStyle::None && endCap != LineCapStyle::None);
    const double maxHeadLen = dualCaps
        ? (std::max)(4.0, (totalLen - static_cast<double>(thick) * 1.2) * 0.42)
        : (totalLen * 0.45);
    const double lookbackDist = (std::min)(maxHeadLen, (std::max)(10.0, static_cast<double>(thick) * 3.0));

    // 计算起点外指切线方向 (从内部指向 pts.front())
    cv::Point startRef = pts[1];
    double acc = 0.0;
    for (size_t i = 1; i < pts.size(); ++i) {
        acc += std::hypot(pts[i].x - pts[i - 1].x, pts[i].y - pts[i - 1].y);
        startRef = pts[i];
        if (acc >= lookbackDist) break;
    }
    const double startAngle = std::atan2(static_cast<double>(pts.front().y - startRef.y),
                                         static_cast<double>(pts.front().x - startRef.x));

    // 计算终点外指切线方向 (从内部指向 pts.back())
    cv::Point endRef = pts[pts.size() - 2];
    acc = 0.0;
    for (size_t i = pts.size() - 1; i > 0; --i) {
        acc += std::hypot(pts[i].x - pts[i - 1].x, pts[i].y - pts[i - 1].y);
        endRef = pts[i - 1];
        if (acc >= lookbackDist) break;
    }
    const double endAngle = std::atan2(static_cast<double>(pts.back().y - endRef.y),
                                       static_cast<double>(pts.back().x - endRef.x));

    float startTrim = 0.0f;
    float endTrim = 0.0f;
    drawEndpointCap(canvas, pts.front(), startAngle, startCap, color, thick, maxHeadLen, startTrim);
    drawEndpointCap(canvas, pts.back(), endAngle, endCap, color, thick, maxHeadLen, endTrim);

    auto shaftPts = trimPolylineEnds(pts, startTrim, endTrim);
    if (shaftPts.size() >= 2) {
        drawPatternedPolyline(canvas, shaftPts, false, color, thick, element.lineStyle);
    }
}

/// 渲染 90 后人教版初高中历史课本地图经典行军路线箭头：
///   1. ArrowStyle::CampaignMarch: 「战役立体」中轴阴阳双色立体浮雕分水岭 + 指数喇叭渐宽飘带 + 深 V 燕尾起点 + 刀锋锐角后掠大箭头（三大战役/渡江战役同款）
///   2. ArrowStyle::MarchDoubleTrack: 「长征路线」根据地同心圆环起点 ◎ + 通透高反差双轨行军通道 + 沿途内嵌连续行军方向流向标 ──▸──▸──▸ + 终点会师燕尾主箭头（红军长征示意图同款）
void renderHistoricalMarchArrow(cv::Mat& canvas, const MarkupElement& element) {
    auto rawPts = buildLineOrCurvePath(element);
    if (rawPts.size() < 2) return;

    // 若为两点直线，自动细分 24 段采样点以确保渐粗飘带包络与平滑曲线一样丝滑
    std::vector<cv::Point2f> pts;
    if (rawPts.size() == 2) {
        const int segs = 24;
        pts.reserve(segs + 1);
        for (int i = 0; i <= segs; ++i) {
            const float t = static_cast<float>(i) / static_cast<float>(segs);
            pts.emplace_back(rawPts[0].x + (rawPts[1].x - rawPts[0].x) * t,
                             rawPts[0].y + (rawPts[1].y - rawPts[0].y) * t);
        }
    } else {
        pts.reserve(rawPts.size());
        for (const auto& p : rawPts) {
            pts.emplace_back(static_cast<float>(p.x), static_cast<float>(p.y));
        }
    }

    std::vector<float> cumLen(pts.size(), 0.0f);
    for (size_t i = 1; i < pts.size(); ++i) {
        cumLen[i] = cumLen[i - 1] + std::hypot(pts[i].x - pts[i - 1].x, pts[i].y - pts[i - 1].y);
    }
    const float totalLen = cumLen.back();
    if (totalLen < 4.0f) return;

    const float thick = std::max(2.0f, element.thickness);
    const bool isCampaign = (element.arrowStyle == ArrowStyle::CampaignMarch);

    auto samplePointAndTangent = [&](float targetDist, cv::Point2f& outPt, cv::Point2f& outTan) {
        targetDist = std::clamp(targetDist, 0.0f, totalLen);
        size_t segIdx = 1;
        for (size_t i = 1; i < pts.size(); ++i) {
            if (cumLen[i] >= targetDist || i == pts.size() - 1) {
                segIdx = i;
                break;
            }
        }
        const float segLen = cumLen[segIdx] - cumLen[segIdx - 1];
        const float r = (segLen > 1e-4f) ? std::clamp((targetDist - cumLen[segIdx - 1]) / segLen, 0.0f, 1.0f) : 0.0f;
        outPt = cv::Point2f(pts[segIdx - 1].x + (pts[segIdx].x - pts[segIdx - 1].x) * r,
                            pts[segIdx - 1].y + (pts[segIdx].y - pts[segIdx - 1].y) * r);

        const float d1 = std::max(0.0f, targetDist - 4.0f);
        const float d2 = std::min(totalLen, targetDist + 4.0f);
        auto interpOnly = [&](float dVal) -> cv::Point2f {
            for (size_t i = 1; i < pts.size(); ++i) {
                if (cumLen[i] >= dVal || i == pts.size() - 1) {
                    const float sl = cumLen[i] - cumLen[i - 1];
                    const float rt = (sl > 1e-4f) ? std::clamp((dVal - cumLen[i - 1]) / sl, 0.0f, 1.0f) : 0.0f;
                    return cv::Point2f(pts[i - 1].x + (pts[i].x - pts[i - 1].x) * rt,
                                       pts[i - 1].y + (pts[i].y - pts[i - 1].y) * rt);
                }
            }
            return pts.back();
        };
        const cv::Point2f pBefore = interpOnly(d1);
        const cv::Point2f pAfter = interpOnly(d2);
        float tx = pAfter.x - pBefore.x;
        float ty = pAfter.y - pBefore.y;
        const float tNorm = std::hypot(tx, ty);
        if (tNorm > 1e-4f) {
            outTan = cv::Point2f(tx / tNorm, ty / tNorm);
        } else {
            outTan = cv::Point2f(1.0f, 0.0f);
        }
    };

    constexpr int kSubPixelShift = 4;
    constexpr float kSubPixelScale = static_cast<float>(1 << kSubPixelShift);
    auto toSubPixelPt = [&](const cv::Point2f& p) -> cv::Point {
        return cv::Point(static_cast<int>(std::lround(p.x * kSubPixelScale)),
                         static_cast<int>(std::lround(p.y * kSubPixelScale)));
    };
    auto toSubPixelPoly = [&](const std::vector<cv::Point2f>& src) -> std::vector<cv::Point> {
        std::vector<cv::Point> res;
        res.reserve(src.size());
        for (const auto& p : src) {
            res.push_back(toSubPixelPt(p));
        }
        return res;
    };

    // 计算阴阳双色立体浮雕色调（向光亮面 + 背光暗面 + 高反差图廓镶边）
    const int baseR = element.color.r;
    const int baseG = element.color.g;
    const int baseB = element.color.b;
    const float lum = (0.299f * baseR + 0.587f * baseG + 0.114f * baseB) / 255.0f;

    const cv::Scalar mainColor = element.color.toCvScalar();
    const cv::Scalar litBevelColor(
        std::clamp(static_cast<int>(std::lround(baseB + (255 - baseB) * 0.24f)), 0, 255),
        std::clamp(static_cast<int>(std::lround(baseG + (255 - baseG) * 0.24f)), 0, 255),
        std::clamp(static_cast<int>(std::lround(baseR + (255 - baseR) * 0.24f)), 0, 255),
        255);
    const cv::Scalar shadeBevelColor(
        std::clamp(static_cast<int>(std::lround(baseB * 0.70f)), 0, 255),
        std::clamp(static_cast<int>(std::lround(baseG * 0.70f)), 0, 255),
        std::clamp(static_cast<int>(std::lround(baseR * 0.70f)), 0, 255),
        255);
    const cv::Scalar ridgeColor(
        std::clamp(static_cast<int>(std::lround(baseB + (255 - baseB) * 0.55f)), 0, 255),
        std::clamp(static_cast<int>(std::lround(baseG + (255 - baseG) * 0.55f)), 0, 255),
        std::clamp(static_cast<int>(std::lround(baseR + (255 - baseR) * 0.55f)), 0, 255),
        255);
    const cv::Scalar mapBorderColor = !element.textOutlineColor.isAuto()
        ? element.textOutlineColor.toCvScalar()
        : ((lum > 0.78f)
               ? cv::Scalar(36, 20, 12, 255)
               : cv::Scalar(255, 255, 255, 255));
    const float borderOut = element.hasCustomOutline
        ? (std::max(0.0f, element.outlineThickness) * 0.75f)
        : 1.5f;

    if (isCampaign) {
        // ─────────────────────────────────────────────────────────────────────────
        // 样式 1：人教版历史地图「战役立体」进攻大箭头 (CampaignMarch)
        //   - 指数级喇叭渐粗箭杆 + 深 V 燕尾起点 + 后掠倒刺刀锋大箭头
        //   - 法向外扩几何图廓底衬（100% 保持刀锋锐角，绝不把尖端磨成圆球）
        //   - 中轴分水岭阴阳双色立体浮雕着色（左翼亮面 + 右翼暗面 + 中轴高光棱线）
        // ─────────────────────────────────────────────────────────────────────────
        const float headLen = std::clamp(std::max(26.0f, thick * 5.8f), 16.0f, totalLen * 0.45f);
        const float headWingHalfW = headLen * 0.60f;
        const float neckBackDist = headLen * 0.72f;
        const float shaftEndDist = std::max(2.0f, totalLen - neckBackDist);
        const float neckHalfW = std::max(3.4f, headWingHalfW * 0.42f);
        const float tailHalfW = std::max(1.6f, neckHalfW * 0.28f);

        const int shaftSteps = 24;
        std::vector<cv::Point2f> spineTrack;
        std::vector<cv::Point2f> leftTrack, rightTrack;
        std::vector<cv::Point2f> leftOuterTrack, rightOuterTrack;
        spineTrack.reserve(shaftSteps + 1);
        leftTrack.reserve(shaftSteps + 1);
        rightTrack.reserve(shaftSteps + 1);
        leftOuterTrack.reserve(shaftSteps + 1);
        rightOuterTrack.reserve(shaftSteps + 1);

        for (int i = 0; i <= shaftSteps; ++i) {
            const float u = static_cast<float>(i) / static_cast<float>(shaftSteps);
            const float s = u * shaftEndDist;
            cv::Point2f sp, tanV;
            samplePointAndTangent(s, sp, tanV);
            const cv::Point2f normV(-tanV.y, tanV.x);
            const float halfW = tailHalfW + (neckHalfW - tailHalfW) * std::pow(u, 1.15f);
            const float outW = halfW + borderOut;
            cv::Point2f backShift = (i == 0) ? cv::Point2f(-tanV.x * borderOut, -tanV.y * borderOut) : cv::Point2f(0.0f, 0.0f);
            spineTrack.push_back(sp);
            leftTrack.emplace_back(sp.x + normV.x * halfW, sp.y + normV.y * halfW);
            rightTrack.emplace_back(sp.x - normV.x * halfW, sp.y - normV.y * halfW);
            leftOuterTrack.emplace_back(sp.x + normV.x * outW + backShift.x, sp.y + normV.y * outW + backShift.y);
            rightOuterTrack.emplace_back(sp.x - normV.x * outW + backShift.x, sp.y - normV.y * outW + backShift.y);
        }

        const cv::Point2f tipPt = pts.back();
        cv::Point2f endRefPt, endTan;
        samplePointAndTangent(std::max(0.0f, totalLen - std::min(headLen * 0.45f, totalLen * 0.22f)), endRefPt, endTan);
        const float tipDx = tipPt.x - endRefPt.x;
        const float tipDy = tipPt.y - endRefPt.y;
        const float tipNorm = std::hypot(tipDx, tipDy);
        if (tipNorm > 1e-3f) {
            endTan = cv::Point2f(tipDx / tipNorm, tipDy / tipNorm);
        }
        const cv::Point2f endNorm(-endTan.y, endTan.x);
        const cv::Point2f wingBase(tipPt.x - endTan.x * headLen, tipPt.y - endTan.y * headLen);
        const cv::Point2f leftWing(wingBase.x + endNorm.x * headWingHalfW, wingBase.y + endNorm.y * headWingHalfW);
        const cv::Point2f rightWing(wingBase.x - endNorm.x * headWingHalfW, wingBase.y - endNorm.y * headWingHalfW);

        const cv::Point2f outerTip(tipPt.x + endTan.x * (borderOut * 1.65f), tipPt.y + endTan.y * (borderOut * 1.65f));
        const cv::Point2f outerLeftWing(leftWing.x + (endNorm.x - endTan.x * 0.45f) * (borderOut * 1.35f),
                                        leftWing.y + (endNorm.y - endTan.y * 0.45f) * (borderOut * 1.35f));
        const cv::Point2f outerRightWing(rightWing.x + (-endNorm.x - endTan.x * 0.45f) * (borderOut * 1.35f),
                                         rightWing.y + (-endNorm.y - endTan.y * 0.45f) * (borderOut * 1.35f));

        cv::Point2f startPt, startTan;
        samplePointAndTangent(0.0f, startPt, startTan);
        const float tailNotchDepth = std::min(shaftEndDist * 0.22f, std::max(4.8f, tailHalfW * 2.25f));
        const cv::Point2f tailNotchPt(startPt.x + startTan.x * tailNotchDepth,
                                      startPt.y + startTan.y * tailNotchDepth);
        const cv::Point2f outerNotchPt(tailNotchPt.x - startTan.x * (borderOut * 1.1f),
                                       tailNotchPt.y - startTan.y * (borderOut * 1.1f));

        // (1) 刀锋几何外扩图廓衬底（保持尖角 100% 锐利挺拔；当 borderOut <= 0.05f 时支持无边框纯净绘制）
        if (borderOut > 0.05f) {
            std::vector<cv::Point2f> outerPoly;
            outerPoly.reserve(leftOuterTrack.size() + rightOuterTrack.size() + 5);
            for (const auto& p : leftOuterTrack) outerPoly.push_back(p);
            outerPoly.push_back(outerLeftWing);
            outerPoly.push_back(outerTip);
            outerPoly.push_back(outerRightWing);
            for (int i = static_cast<int>(rightOuterTrack.size()) - 1; i >= 0; --i) {
                outerPoly.push_back(rightOuterTrack[static_cast<size_t>(i)]);
            }
            outerPoly.push_back(outerNotchPt);
            const auto outerFixed = toSubPixelPoly(outerPoly);
            cv::fillPoly(canvas, std::vector<std::vector<cv::Point>>{outerFixed}, mapBorderColor, cv::LINE_AA, kSubPixelShift);
        }

        // (2) 向光半翼多边形 (Left Bevel: tailNotchPt -> leftTrack -> leftWing -> tipPt -> spine[::-1])
        std::vector<cv::Point2f> leftHalfPoly;
        leftHalfPoly.reserve(leftTrack.size() + spineTrack.size() + 4);
        leftHalfPoly.push_back(tailNotchPt);
        for (const auto& p : leftTrack) leftHalfPoly.push_back(p);
        leftHalfPoly.push_back(leftWing);
        leftHalfPoly.push_back(tipPt);
        for (int i = static_cast<int>(spineTrack.size()) - 1; i >= 1; --i) {
            leftHalfPoly.push_back(spineTrack[static_cast<size_t>(i)]);
        }
        const auto leftFixed = toSubPixelPoly(leftHalfPoly);
        cv::fillPoly(canvas, std::vector<std::vector<cv::Point>>{leftFixed}, litBevelColor, cv::LINE_AA, kSubPixelShift);

        // (3) 背光半翼多边形 (Right Bevel: tailNotchPt -> spine[1..N] -> tipPt -> rightWing -> rightTrack[::-1])
        std::vector<cv::Point2f> rightHalfPoly;
        rightHalfPoly.reserve(rightTrack.size() + spineTrack.size() + 4);
        rightHalfPoly.push_back(tailNotchPt);
        for (size_t i = 1; i < spineTrack.size(); ++i) {
            rightHalfPoly.push_back(spineTrack[i]);
        }
        rightHalfPoly.push_back(tipPt);
        rightHalfPoly.push_back(rightWing);
        for (int i = static_cast<int>(rightTrack.size()) - 1; i >= 0; --i) {
            rightHalfPoly.push_back(rightTrack[static_cast<size_t>(i)]);
        }
        const auto rightFixed = toSubPixelPoly(rightHalfPoly);
        cv::fillPoly(canvas, std::vector<std::vector<cv::Point>>{rightFixed}, shadeBevelColor, cv::LINE_AA, kSubPixelShift);

        // (4) 中轴立体分水岭棱线 (Central Ridge Highlight)
        std::vector<cv::Point2f> ridgePts;
        ridgePts.reserve(spineTrack.size() + 2);
        ridgePts.push_back(tailNotchPt);
        for (size_t i = 1; i < spineTrack.size(); ++i) ridgePts.push_back(spineTrack[i]);
        ridgePts.push_back(tipPt);
        const auto ridgeFixed = toSubPixelPoly(ridgePts);
        cv::polylines(canvas, std::vector<std::vector<cv::Point>>{ridgeFixed}, false, ridgeColor, 1, cv::LINE_AA, kSubPixelShift);
    } else {
        // ─────────────────────────────────────────────────────────────────────────
        // 样式 2：人教版《中国工农红军长征示意图》「长征路线」流向行军箭头 (MarchDoubleTrack)
        //   - 起点根据地同心圆环靶标 ◎（象征瑞金/出发根据地）
        //   - 高反差明亮双轨通道（外侧双轨主色实线 + 中间清爽亮白/浅主色通道底衬）
        //   - 沿途内嵌连续行军方向小箭标 ──▸──▸──▸（清晰标示弯曲路线行进流向）
        //   - 终点独立刀锋燕尾实心会师主箭头
        // ─────────────────────────────────────────────────────────────────────────
        const float trackHalfW = std::clamp(thick * 0.78f + 2.2f, 3.6f, 8.8f);
        const float headLen = std::clamp(std::max(24.0f, trackHalfW * 4.2f), 14.0f, totalLen * 0.42f);
        const float headWingHalfW = headLen * 0.58f;
        const float neckBackDist = headLen * 0.72f;
        const float shaftEndDist = std::max(2.0f, totalLen - neckBackDist);

        const int shaftSteps = 24;
        std::vector<cv::Point2f> leftTrack, rightTrack;
        leftTrack.reserve(shaftSteps + 1);
        rightTrack.reserve(shaftSteps + 1);
        for (int i = 0; i <= shaftSteps; ++i) {
            const float u = static_cast<float>(i) / static_cast<float>(shaftSteps);
            cv::Point2f sp, tanV;
            samplePointAndTangent(u * shaftEndDist, sp, tanV);
            const cv::Point2f normV(-tanV.y, tanV.x);
            leftTrack.emplace_back(sp.x + normV.x * trackHalfW, sp.y + normV.y * trackHalfW);
            rightTrack.emplace_back(sp.x - normV.x * trackHalfW, sp.y - normV.y * trackHalfW);
        }

        // (1) 双轨通道底衬：通透明亮的高反差浅色内芯（告别灰暗浑浊）
        std::vector<cv::Point2f> corridorPoly;
        corridorPoly.reserve(leftTrack.size() + rightTrack.size());
        for (const auto& p : leftTrack) corridorPoly.push_back(p);
        for (int i = static_cast<int>(rightTrack.size()) - 1; i >= 0; --i) {
            corridorPoly.push_back(rightTrack[static_cast<size_t>(i)]);
        }
        const cv::Scalar corridorFill(
            std::clamp(static_cast<int>(std::lround(248 * 0.80f + baseB * 0.20f)), 0, 255),
            std::clamp(static_cast<int>(std::lround(250 * 0.80f + baseG * 0.20f)), 0, 255),
            std::clamp(static_cast<int>(std::lround(252 * 0.80f + baseR * 0.20f)), 0, 255),
            255);
        const auto corridorFixed = toSubPixelPoly(corridorPoly);
        cv::fillPoly(canvas, std::vector<std::vector<cv::Point>>{corridorFixed}, corridorFill, cv::LINE_AA, kSubPixelShift);

        // (2) 两条平行主色护轨实线
        const int railThick = std::max(2, static_cast<int>(std::lround(std::clamp(thick * 0.46f, 1.8f, 3.4f))));
        const auto leftRailFixed = toSubPixelPoly(leftTrack);
        const auto rightRailFixed = toSubPixelPoly(rightTrack);
        if (borderOut > 0.05f) {
            const int railBorderExtra = std::max(1, static_cast<int>(std::lround(borderOut * 1.4f)));
            cv::polylines(canvas, std::vector<std::vector<cv::Point>>{leftRailFixed}, false, mapBorderColor, railThick + railBorderExtra, cv::LINE_AA, kSubPixelShift);
            cv::polylines(canvas, std::vector<std::vector<cv::Point>>{rightRailFixed}, false, mapBorderColor, railThick + railBorderExtra, cv::LINE_AA, kSubPixelShift);
        }
        cv::polylines(canvas, std::vector<std::vector<cv::Point>>{leftRailFixed}, false, mainColor, railThick, cv::LINE_AA, kSubPixelShift);
        cv::polylines(canvas, std::vector<std::vector<cv::Point>>{rightRailFixed}, false, mainColor, railThick, cv::LINE_AA, kSubPixelShift);

        // (3) 沿途内嵌连续行军方向流向小箭标 (Inline March Chevrons ──▸──▸──▸)
        const float originR = std::clamp(trackHalfW * 1.35f, 5.0f, 12.0f);
        const float chevLen = std::clamp(trackHalfW * 1.65f, 6.0f, 14.0f);
        const float chevWingW = trackHalfW * 0.64f;
        const float stepDist = std::max(26.0f, trackHalfW * 5.2f);
        const float startMargin = originR + chevLen * 0.9f;
        const float endMargin = headLen * 0.45f;
        for (float d = startMargin + stepDist * 0.45f; d + chevLen * 0.5f < shaftEndDist - endMargin; d += stepDist) {
            cv::Point2f cPt, cTan;
            samplePointAndTangent(d, cPt, cTan);
            const cv::Point2f cNorm(-cTan.y, cTan.x);
            const cv::Point2f cTip(cPt.x + cTan.x * (chevLen * 0.55f), cPt.y + cTan.y * (chevLen * 0.55f));
            const cv::Point2f cBase(cPt.x - cTan.x * (chevLen * 0.45f), cPt.y - cTan.y * (chevLen * 0.45f));
            const cv::Point2f cNotch(cPt.x - cTan.x * (chevLen * 0.18f), cPt.y - cTan.y * (chevLen * 0.18f));
            const cv::Point2f cLW(cBase.x + cNorm.x * chevWingW, cBase.y + cNorm.y * chevWingW);
            const cv::Point2f cRW(cBase.x - cNorm.x * chevWingW, cBase.y - cNorm.y * chevWingW);
            const auto chevFixed = toSubPixelPoly({ cTip, cLW, cNotch, cRW });
            cv::fillPoly(canvas, std::vector<std::vector<cv::Point>>{chevFixed}, mainColor, cv::LINE_AA, kSubPixelShift);
        }

        // (4) 起点根据地同心圆环靶标 ◎ (Base Camp Origin Bullseye)
        const cv::Point startSub = toSubPixelPt(pts.front());
        const int rOuterSub = static_cast<int>(std::lround((originR + borderOut) * kSubPixelScale));
        const int rMainSub = static_cast<int>(std::lround(originR * kSubPixelScale));
        const int rInnerWhiteSub = static_cast<int>(std::lround((originR * 0.62f) * kSubPixelScale));
        const int rCoreDotSub = static_cast<int>(std::lround((originR * 0.32f) * kSubPixelScale));
        if (borderOut > 0.05f) {
            cv::circle(canvas, startSub, rOuterSub, mapBorderColor, cv::FILLED, cv::LINE_AA, kSubPixelShift);
        }
        cv::circle(canvas, startSub, rMainSub, mainColor, cv::FILLED, cv::LINE_AA, kSubPixelShift);
        cv::circle(canvas, startSub, rInnerWhiteSub, cv::Scalar(255, 255, 255, 255), cv::FILLED, cv::LINE_AA, kSubPixelShift);
        cv::circle(canvas, startSub, rCoreDotSub, mainColor, cv::FILLED, cv::LINE_AA, kSubPixelShift);

        // (5) 终点独立刀锋燕尾会师主箭头
        const cv::Point2f tipPt = pts.back();
        cv::Point2f endRefPt, endTan;
        samplePointAndTangent(std::max(0.0f, totalLen - std::min(headLen * 0.45f, totalLen * 0.22f)), endRefPt, endTan);
        const float tipDx = tipPt.x - endRefPt.x;
        const float tipDy = tipPt.y - endRefPt.y;
        const float tipNorm = std::hypot(tipDx, tipDy);
        if (tipNorm > 1e-3f) {
            endTan = cv::Point2f(tipDx / tipNorm, tipDy / tipNorm);
        }
        const cv::Point2f endNorm(-endTan.y, endTan.x);
        const cv::Point2f wingBase(tipPt.x - endTan.x * headLen, tipPt.y - endTan.y * headLen);
        const cv::Point2f headNotch(tipPt.x - endTan.x * (headLen * 0.72f), tipPt.y - endTan.y * (headLen * 0.72f));
        const cv::Point2f leftWing(wingBase.x + endNorm.x * headWingHalfW, wingBase.y + endNorm.y * headWingHalfW);
        const cv::Point2f rightWing(wingBase.x - endNorm.x * headWingHalfW, wingBase.y - endNorm.y * headWingHalfW);

        const float bOut = borderOut;
        if (bOut > 0.05f) {
            const cv::Point2f oTip(tipPt.x + endTan.x * (bOut * 1.6f), tipPt.y + endTan.y * (bOut * 1.6f));
            const cv::Point2f oLW(leftWing.x + (endNorm.x - endTan.x * 0.4f) * (bOut * 1.35f),
                                  leftWing.y + (endNorm.y - endTan.y * 0.4f) * (bOut * 1.35f));
            const cv::Point2f oRW(rightWing.x + (-endNorm.x - endTan.x * 0.4f) * (bOut * 1.35f),
                                  rightWing.y + (-endNorm.y - endTan.y * 0.4f) * (bOut * 1.35f));
            const cv::Point2f oNotch(headNotch.x - endTan.x * bOut, headNotch.y - endTan.y * bOut);
            const auto headOuterFixed = toSubPixelPoly({ oTip, oLW, oNotch, oRW });
            cv::fillPoly(canvas, std::vector<std::vector<cv::Point>>{headOuterFixed}, mapBorderColor, cv::LINE_AA, kSubPixelShift);
        }
        const auto headLeftFixed = toSubPixelPoly({ tipPt, leftWing, headNotch });
        const auto headRightFixed = toSubPixelPoly({ tipPt, headNotch, rightWing });
        cv::fillPoly(canvas, std::vector<std::vector<cv::Point>>{headLeftFixed}, litBevelColor, cv::LINE_AA, kSubPixelShift);
        cv::fillPoly(canvas, std::vector<std::vector<cv::Point>>{headRightFixed}, shadeBevelColor, cv::LINE_AA, kSubPixelShift);
    }
}

std::vector<cv::Point> generateRoundedRectContour(int x1, int y1, int x2, int y2, float radius) {
    int left = (std::min)(x1, x2);
    int top = (std::min)(y1, y2);
    int right = (std::max)(x1, x2);
    int bottom = (std::max)(y1, y2);
    int w = right - left;
    int h = bottom - top;

    float r = std::clamp(radius, 0.0f, static_cast<float>((std::min)(w, h)) * 0.5f);
    if (r <= 0.5f) {
        return { cv::Point(left, top), cv::Point(right, top), cv::Point(right, bottom), cv::Point(left, bottom) };
    }

    std::vector<cv::Point> pts;
    const int arcSegments = 8;

    auto addArc = [&](float cx, float cy, float startAngle, float endAngle) {
        for (int i = 0; i <= arcSegments; ++i) {
            float t = static_cast<float>(i) / arcSegments;
            float angle = startAngle + t * (endAngle - startAngle);
            pts.emplace_back(static_cast<int>(std::round(cx + r * std::cos(angle))),
                             static_cast<int>(std::round(cy + r * std::sin(angle))));
        }
    };

    addArc(right - r, top + r, -static_cast<float>(CV_PI) * 0.5f, 0.0f);
    addArc(right - r, bottom - r, 0.0f, static_cast<float>(CV_PI) * 0.5f);
    addArc(left + r, bottom - r, static_cast<float>(CV_PI) * 0.5f, static_cast<float>(CV_PI));
    addArc(left + r, top + r, static_cast<float>(CV_PI), static_cast<float>(CV_PI) * 1.5f);

    return pts;
}

void drawPatternedRect(cv::Mat& canvas, cv::Point p1, cv::Point p2, const cv::Scalar& color, int thick, LineStyle style, bool fill, float cornerRadius = 0.0f) {
    int x1 = (std::min)(p1.x, p2.x);
    int y1 = (std::min)(p1.y, p2.y);
    int x2 = (std::max)(p1.x, p2.x);
    int y2 = (std::max)(p1.y, p2.y);

    if (x2 <= x1 || y2 <= y1) return;

    if (cornerRadius <= 0.5f) {
        if (fill) {
            cv::rectangle(canvas, cv::Point(x1, y1), cv::Point(x2, y2), color, cv::FILLED);
        }
        if (style == LineStyle::Solid) {
            if (!fill || thick > 1) {
                cv::rectangle(canvas, cv::Point(x1, y1), cv::Point(x2, y2), color, thick, cv::LINE_AA);
            }
        } else {
            std::vector<cv::Point> pts = { cv::Point(x1, y1), cv::Point(x2, y1), cv::Point(x2, y2), cv::Point(x1, y2) };
            drawPatternedPolyline(canvas, pts, true, color, thick, style);
        }
    } else {
        auto contour = generateRoundedRectContour(x1, y1, x2, y2, cornerRadius);
        if (fill) {
            std::vector<std::vector<cv::Point>> polys = { contour };
            cv::fillPoly(canvas, polys, color, cv::LINE_AA);
        }
        if (!fill || thick > 1) {
            drawPatternedPolyline(canvas, contour, true, color, thick, style);
        }
    }
}

class RectangleHandler : public DefaultBoxHandler {
public:
    void render(cv::Mat& canvas, const MarkupElement& element) const override {
        auto color = element.color.toCvScalar();
        int thick = (std::max)(1, static_cast<int>(element.thickness));
        if (element.fill && (element.hasCustomOutline || !element.textOutlineColor.isAuto())) {
            // 1. 先绘制实心填充底面 (thick=1 且 Solid 仅做内部填充)
            int x1 = (std::min)(element.startPt.x, element.endPt.x);
            int y1 = (std::min)(element.startPt.y, element.endPt.y);
            int x2 = (std::max)(element.startPt.x, element.endPt.x);
            int y2 = (std::max)(element.startPt.y, element.endPt.y);
            if (x2 <= x1 || y2 <= y1) return;
            if (element.cornerRadius <= 0.5f) {
                cv::rectangle(canvas, cv::Point(x1, y1), cv::Point(x2, y2), color, cv::FILLED);
            } else {
                auto contour = generateRoundedRectContour(x1, y1, x2, y2, element.cornerRadius);
                std::vector<std::vector<cv::Point>> polys = { contour };
                cv::fillPoly(canvas, polys, color, cv::LINE_AA);
            }
            // 2. 再按独立边框粗细与边框颜色绘制外框（outlineThickness <= 0.05f 时为纯色无边框块）
            const float outW = element.hasCustomOutline ? element.outlineThickness : element.thickness;
            if (outW > 0.05f) {
                const int borderThick = (std::max)(1, static_cast<int>(std::lround(outW)));
                const cv::Scalar borderCol = !element.textOutlineColor.isAuto()
                    ? element.textOutlineColor.toCvScalar()
                    : color;
                drawPatternedRect(canvas, element.startPt, element.endPt, borderCol, borderThick, element.lineStyle, false, element.cornerRadius);
            }
            return;
        }
        drawPatternedRect(canvas, element.startPt, element.endPt, color, thick, element.lineStyle, element.fill, element.cornerRadius);
    }

    HitArea hitTest(const MarkupElement& element, cv::Point pt, int padding) const override {
        if (element.isActive) {
            // 1. 优先检测激活态 8 方向拉伸手柄 (统一由 HandleGeometry 管理)
            HitArea handleHit = hitTestHandles(element, pt, padding);
            if (handleHit != HitArea::None) return handleHit;

            // 2. 检测内角圆角调节手柄 (位于角内侧)
            cv::Rect bbox = getBoundingBox(element);
            float w = static_cast<float>(bbox.width);
            float h = static_cast<float>(bbox.height);
            if (CornerRadiusHelper::canShowHandles(w, h, 1.0f, 40.0f)) {
                bool isSmall = (w < 80.0f || h < 80.0f);
                int hitIdx = CornerRadiusHelper::hitTestHandles(
                    static_cast<float>(bbox.x), static_cast<float>(bbox.y),
                    static_cast<float>(bbox.x + bbox.width), static_cast<float>(bbox.y + bbox.height),
                    element.cornerRadius,
                    static_cast<float>(pt.x), static_cast<float>(pt.y),
                    1.0f, 9.0f, isSmall);
                if (hitIdx >= 0) {
                    return HitArea::CornerRadius;
                }
            }
        }

        cv::Rect bbox = getBoundingBox(element);
        int p = (std::max)(padding, 6);
        if (!element.fill) {
            cv::Rect outer(bbox.x - p, bbox.y - p, bbox.width + p * 2, bbox.height + p * 2);
            cv::Rect inner(bbox.x + p, bbox.y + p, (std::max)(0, bbox.width - p * 2), (std::max)(0, bbox.height - p * 2));
            if (outer.contains(pt) && !inner.contains(pt)) {
                return HitArea::Body;
            }
            return HitArea::None;
        }
        bbox.x -= p; bbox.y -= p; bbox.width += p * 2; bbox.height += p * 2;
        if (bbox.contains(pt)) return HitArea::Body;
        return HitArea::None;
    }

    void resize(MarkupElement& element, int dx, int dy, HitArea handle) const override {
        if (handle == HitArea::CornerRadius) {
            int x1 = (std::min)(element.startPt.x, element.endPt.x);
            int y1 = (std::min)(element.startPt.y, element.endPt.y);
            int x2 = (std::max)(element.startPt.x, element.endPt.x);
            int y2 = (std::max)(element.startPt.y, element.endPt.y);
            float w = static_cast<float>(x2 - x1);
            float h = static_cast<float>(y2 - y1);
            float maxR = (std::min)(w, h) * 0.5f;
            element.cornerRadius = CornerRadiusHelper::applyIncrementalDrag(
                element.cornerRadius, static_cast<float>(dx), static_cast<float>(dy), 1.0f, 1.0f, maxR);
        } else {
            DefaultBoxHandler::resize(element, dx, dy, handle);
            int w = element.endPt.x - element.startPt.x;
            int h = element.endPt.y - element.startPt.y;
            float maxR = (std::min)(w, h) * 0.5f;
            if (element.cornerRadius > maxR) {
                element.cornerRadius = maxR;
            }
        }
        LOG_DEBUG("MarkupEngine: RectangleHandler::resize handle={}, dx={}, dy={}, radius={:.1f}",
                  static_cast<int>(handle), dx, dy, element.cornerRadius);
    }

    void renderActiveHandles(cv::Mat& canvas, const MarkupElement& element) const override {
        // 仅绘制外围 8 个标准缩放控制点；内角圆角微晶手柄统一交由 CaptureRenderer 在 Direct2D 层按近场感应渐显绘制
        DefaultBoxHandler::renderActiveHandles(canvas, element);
    }
};

double pointToPolylineMinDistance(const std::vector<cv::Point>& pts, cv::Point pt) {
    if (pts.empty()) return 1e9;
    if (pts.size() == 1) {
        return std::hypot(pt.x - pts[0].x, pt.y - pts[0].y);
    }
    double minDist = 1e9;
    for (size_t i = 1; i < pts.size(); ++i) {
        const double dx = pts[i].x - pts[i - 1].x;
        const double dy = pts[i].y - pts[i - 1].y;
        const double lenSq = dx * dx + dy * dy;
        double d = 0.0;
        if (lenSq < 1e-6) {
            d = std::hypot(pt.x - pts[i - 1].x, pt.y - pts[i - 1].y);
        } else {
            const double t = std::clamp(((pt.x - pts[i - 1].x) * dx + (pt.y - pts[i - 1].y) * dy) / lenSq, 0.0, 1.0);
            const double px = pts[i - 1].x + t * dx;
            const double py = pts[i - 1].y + t * dy;
            d = std::hypot(pt.x - px, pt.y - py);
        }
        if (d < minDist) {
            minDist = d;
        }
    }
    return minDist;
}

class LineHandler : public IMarkupToolHandler {
public:
    cv::Rect getBoundingBox(const MarkupElement& element) const override {
        int x1 = (std::min)(element.startPt.x, element.endPt.x);
        int y1 = (std::min)(element.startPt.y, element.endPt.y);
        int x2 = (std::max)(element.startPt.x, element.endPt.x);
        int y2 = (std::max)(element.startPt.y, element.endPt.y);
        const auto pathPts = (element.smoothCurve || element.penPoints.size() >= 3)
            ? buildLineOrCurvePath(element)
            : element.penPoints;
        for (const auto& pt : pathPts) {
            x1 = (std::min)(x1, pt.x);
            y1 = (std::min)(y1, pt.y);
            x2 = (std::max)(x2, pt.x);
            y2 = (std::max)(y2, pt.y);
        }
        int pad = (std::max)(1, static_cast<int>(std::ceil(element.thickness / 2.0f)));
        if (element.hasCustomCaps && (element.startCap != LineCapStyle::None || element.endCap != LineCapStyle::None)) {
            pad = (std::max)(pad, static_cast<int>(std::ceil(element.thickness * 2.5f)));
        }
        x1 -= pad; y1 -= pad;
        x2 += pad; y2 += pad;
        return cv::Rect(x1, y1, x2 - x1, y2 - y1);
    }

    HitArea hitTest(const MarkupElement& element, cv::Point pt, int padding) const override {
        if (element.isActive) {
            int hw = 8;
            cv::Rect h1(element.startPt.x - hw, element.startPt.y - hw, hw * 2, hw * 2);
            if (h1.contains(pt)) return HitArea::LT;
            cv::Rect h2(element.endPt.x - hw, element.endPt.y - hw, hw * 2, hw * 2);
            if (h2.contains(pt)) return HitArea::RB;
            if (std::hypot(element.endPt.x - element.startPt.x, element.endPt.y - element.startPt.y) >= 28.0) {
                const cv::Point midH = getArrowOrLineCurveHandle(element);
                cv::Rect hm(midH.x - hw, midH.y - hw, hw * 2, hw * 2);
                if (hm.contains(pt)) return HitArea::T;
            }
        }
        double dist = 0.0;
        if (element.smoothCurve || element.penPoints.size() >= 3) {
            dist = pointToPolylineMinDistance(buildLineOrCurvePath(element), pt);
        } else {
            double dx = element.endPt.x - element.startPt.x;
            double dy = element.endPt.y - element.startPt.y;
            double lenSq = dx * dx + dy * dy;
            if (lenSq < 1e-6) {
                dist = std::hypot(pt.x - element.startPt.x, pt.y - element.startPt.y);
            } else {
                double t = std::clamp(((pt.x - element.startPt.x) * dx + (pt.y - element.startPt.y) * dy) / lenSq, 0.0, 1.0);
                double px = element.startPt.x + t * dx;
                double py = element.startPt.y + t * dy;
                dist = std::hypot(pt.x - px, pt.y - py);
            }
        }
        double tol = (std::max)(static_cast<double>(padding), static_cast<double>(element.thickness) * 0.5 + 6.0);
        if (dist <= tol) return HitArea::Body;
        return HitArea::None;
    }

    void resize(MarkupElement& element, int dx, int dy, HitArea handle) const override {
        if (handle == HitArea::T) {
            const cv::Point curMid = getArrowOrLineCurveHandle(element);
            const cv::Point newMid(curMid.x + dx, curMid.y + dy);
            const cv::Point ctrlPt(
                static_cast<int>(std::lround(2.0f * newMid.x - 0.5f * (element.startPt.x + element.endPt.x))),
                static_cast<int>(std::lround(2.0f * newMid.y - 0.5f * (element.startPt.y + element.endPt.y))));
            element.smoothCurve = true;
            element.penPoints = { element.startPt, ctrlPt, element.endPt };
            return;
        }
        switch (handle) {
            case HitArea::LT:
            case HitArea::L:
            case HitArea::LB:
                element.startPt.x += dx;
                element.startPt.y += dy;
                if (element.penPoints.size() == 3) {
                    element.penPoints[0] = element.startPt;
                    element.penPoints[1].x += dx / 2;
                    element.penPoints[1].y += dy / 2;
                }
                break;
            case HitArea::RB:
            case HitArea::B:
            case HitArea::R:
            case HitArea::RT:
                element.endPt.x += dx;
                element.endPt.y += dy;
                if (element.penPoints.size() == 3) {
                    element.penPoints[2] = element.endPt;
                    element.penPoints[1].x += dx / 2;
                    element.penPoints[1].y += dy / 2;
                }
                break;
            default: break;
        }
    }

    void render(cv::Mat& canvas, const MarkupElement& element) const override {
        if (element.hasCustomCaps || element.smoothCurve || element.penPoints.size() >= 3) {
            const LineCapStyle sCap = element.hasCustomCaps ? element.startCap : LineCapStyle::None;
            const LineCapStyle eCap = element.hasCustomCaps ? element.endCap : LineCapStyle::None;
            renderLineWithEndpointCaps(canvas, element, sCap, eCap);
            return;
        }
        auto color = element.color.toCvScalar();
        int thick = (std::max)(1, static_cast<int>(element.thickness));
        drawPatternedLine(canvas, element.startPt, element.endPt, color, thick, element.lineStyle);
    }

    void renderActiveHandles(cv::Mat& canvas, const MarkupElement& element) const override {
        cv::Scalar activeColor(255, 140, 0); // Azure/Blue
        if (element.smoothCurve || element.penPoints.size() >= 3) {
            drawPatternedPolyline(canvas, buildLineOrCurvePath(element), false, activeColor, 1, LineStyle::Dashed);
        } else {
            drawPatternedLine(canvas, element.startPt, element.endPt, activeColor, 1, LineStyle::Dashed);
        }
        int r = 5;
        cv::Point handles[2] = { element.startPt, element.endPt };
        for (const auto& pt : handles) {
            cv::circle(canvas, pt, r + 1, cv::Scalar(255, 255, 255), cv::FILLED, cv::LINE_AA);
            cv::circle(canvas, pt, r + 1, activeColor, 1, cv::LINE_AA);
        }
        if (std::hypot(element.endPt.x - element.startPt.x, element.endPt.y - element.startPt.y) >= 28.0) {
            const cv::Point midH = getArrowOrLineCurveHandle(element);
            cv::circle(canvas, midH, r + 1, cv::Scalar(255, 255, 255), cv::FILLED, cv::LINE_AA);
            cv::circle(canvas, midH, r - 1, cv::Scalar(36, 190, 251), cv::FILLED, cv::LINE_AA);
            cv::circle(canvas, midH, r + 1, activeColor, 1, cv::LINE_AA);
        }
    }
};

class ArrowHandler : public IMarkupToolHandler {
public:
    cv::Rect getBoundingBox(const MarkupElement& element) const override {
        int x1 = (std::min)(element.startPt.x, element.endPt.x);
        int y1 = (std::min)(element.startPt.y, element.endPt.y);
        int x2 = (std::max)(element.startPt.x, element.endPt.x);
        int y2 = (std::max)(element.startPt.y, element.endPt.y);
        const auto pathPts = (element.smoothCurve || element.penPoints.size() >= 3)
            ? buildLineOrCurvePath(element)
            : element.penPoints;
        for (const auto& pt : pathPts) {
            x1 = (std::min)(x1, pt.x);
            y1 = (std::min)(y1, pt.y);
            x2 = (std::max)(x2, pt.x);
            y2 = (std::max)(y2, pt.y);
        }
        int pad = (std::max)(18, static_cast<int>(std::ceil(element.thickness * 3.2f)));
        x1 -= pad; y1 -= pad;
        x2 += pad; y2 += pad;
        return cv::Rect(x1, y1, x2 - x1, y2 - y1);
    }

    HitArea hitTest(const MarkupElement& element, cv::Point pt, int padding) const override {
        if (element.isActive) {
            int hw = 8;
            cv::Rect h1(element.startPt.x - hw, element.startPt.y - hw, hw * 2, hw * 2);
            if (h1.contains(pt)) return HitArea::LT; // Tail handle
            cv::Rect h2(element.endPt.x - hw, element.endPt.y - hw, hw * 2, hw * 2);
            if (h2.contains(pt)) return HitArea::RB; // Tip handle
            if (std::hypot(element.endPt.x - element.startPt.x, element.endPt.y - element.startPt.y) >= 28.0) {
                const cv::Point midH = getArrowOrLineCurveHandle(element);
                cv::Rect hm(midH.x - hw, midH.y - hw, hw * 2, hw * 2);
                if (hm.contains(pt)) return HitArea::T; // Curve bend handle
            }
        }

        double dist = 0.0;
        if (element.smoothCurve || element.penPoints.size() >= 3) {
            dist = pointToPolylineMinDistance(buildLineOrCurvePath(element), pt);
        } else {
            double dx = element.endPt.x - element.startPt.x;
            double dy = element.endPt.y - element.startPt.y;
            double lenSq = dx * dx + dy * dy;
            if (lenSq < 1e-6) {
                dist = std::hypot(pt.x - element.startPt.x, pt.y - element.startPt.y);
            } else {
                double t = std::clamp(((pt.x - element.startPt.x) * dx + (pt.y - element.startPt.y) * dy) / lenSq, 0.0, 1.0);
                double px = element.startPt.x + t * dx;
                double py = element.startPt.y + t * dy;
                dist = std::hypot(pt.x - px, pt.y - py);
            }
        }

        const bool isMarchStyle = (element.arrowStyle == ArrowStyle::CampaignMarch ||
                                   element.arrowStyle == ArrowStyle::MarchDoubleTrack);
        double headLen = (std::max)(isMarchStyle ? 22.0 : 14.0,
                                    static_cast<double>(element.thickness) * (isMarchStyle ? 5.0 : 3.5));
        double distTip = std::hypot(pt.x - element.endPt.x, pt.y - element.endPt.y);
        if (distTip <= headLen) {
            return HitArea::Body;
        }
        if (element.arrowStyle == ArrowStyle::DoubleEnded ||
            (element.hasCustomCaps && element.startCap != LineCapStyle::None)) {
            double distStart = std::hypot(pt.x - element.startPt.x, pt.y - element.startPt.y);
            if (distStart <= headLen) {
                return HitArea::Body;
            }
        }

        double tol = (std::max)(static_cast<double>(padding),
                                static_cast<double>(element.thickness) * (isMarchStyle ? 1.5 : 0.5) + 8.0);
        if (dist <= tol) return HitArea::Body;
        return HitArea::None;
    }

    void resize(MarkupElement& element, int dx, int dy, HitArea handle) const override {
        if (handle == HitArea::T) {
            const cv::Point curMid = getArrowOrLineCurveHandle(element);
            const cv::Point newMid(curMid.x + dx, curMid.y + dy);
            const cv::Point ctrlPt(
                static_cast<int>(std::lround(2.0f * newMid.x - 0.5f * (element.startPt.x + element.endPt.x))),
                static_cast<int>(std::lround(2.0f * newMid.y - 0.5f * (element.startPt.y + element.endPt.y))));
            element.smoothCurve = true;
            element.penPoints = { element.startPt, ctrlPt, element.endPt };
            return;
        }
        switch (handle) {
            case HitArea::LT:
            case HitArea::L:
            case HitArea::LB:
                element.startPt.x += dx;
                element.startPt.y += dy;
                if (element.penPoints.size() == 3) {
                    element.penPoints[0] = element.startPt;
                    element.penPoints[1].x += dx / 2;
                    element.penPoints[1].y += dy / 2;
                }
                break;
            case HitArea::RB:
            case HitArea::B:
            case HitArea::R:
            case HitArea::RT:
                element.endPt.x += dx;
                element.endPt.y += dy;
                if (element.penPoints.size() == 3) {
                    element.penPoints[2] = element.endPt;
                    element.penPoints[1].x += dx / 2;
                    element.penPoints[1].y += dy / 2;
                }
                break;
            default: break;
        }
    }

    void renderActiveHandles(cv::Mat& canvas, const MarkupElement& element) const override {
        cv::Scalar activeColor(255, 140, 0); // Azure/Blue
        if (element.smoothCurve || element.penPoints.size() >= 3) {
            drawPatternedPolyline(canvas, buildLineOrCurvePath(element), false, activeColor, 1, LineStyle::Dashed);
        } else {
            drawPatternedLine(canvas, element.startPt, element.endPt, activeColor, 1, LineStyle::Dashed);
        }
        int r = 5;
        cv::Point handles[2] = { element.startPt, element.endPt };
        for (const auto& pt : handles) {
            cv::circle(canvas, pt, r + 1, cv::Scalar(255, 255, 255), cv::FILLED, cv::LINE_AA);
            cv::circle(canvas, pt, r + 1, activeColor, 1, cv::LINE_AA);
        }
        if (std::hypot(element.endPt.x - element.startPt.x, element.endPt.y - element.startPt.y) >= 28.0) {
            const cv::Point midH = getArrowOrLineCurveHandle(element);
            cv::circle(canvas, midH, r + 1, cv::Scalar(255, 255, 255), cv::FILLED, cv::LINE_AA);
            cv::circle(canvas, midH, r - 1, cv::Scalar(36, 190, 251), cv::FILLED, cv::LINE_AA);
            cv::circle(canvas, midH, r + 1, activeColor, 1, cv::LINE_AA);
        }
    }

    void render(cv::Mat& canvas, const MarkupElement& element) const override {
        if (element.arrowStyle == ArrowStyle::CampaignMarch ||
            element.arrowStyle == ArrowStyle::MarchDoubleTrack) {
            renderHistoricalMarchArrow(canvas, element);
            return;
        }
        LineCapStyle sCap = LineCapStyle::None;
        LineCapStyle eCap = LineCapStyle::SolidTriangle;
        if (element.hasCustomCaps) {
            sCap = element.startCap;
            eCap = element.endCap;
        } else {
            switch (element.arrowStyle) {
                case ArrowStyle::Thin:
                    sCap = LineCapStyle::None;
                    eCap = LineCapStyle::OpenArrow;
                    break;
                case ArrowStyle::Tech:
                    sCap = LineCapStyle::None;
                    eCap = LineCapStyle::StealthArrow;
                    break;
                case ArrowStyle::DoubleEnded:
                    sCap = LineCapStyle::SolidTriangle;
                    eCap = LineCapStyle::SolidTriangle;
                    break;
                case ArrowStyle::Standard:
                default:
                    sCap = LineCapStyle::None;
                    eCap = LineCapStyle::SolidTriangle;
                    break;
            }
        }
        if ((element.hasCustomOutline || !element.textOutlineColor.isAuto()) &&
            element.outlineThickness > 0.05f) {
            MarkupElement borderElem = element;
            borderElem.color = !element.textOutlineColor.isAuto()
                ? element.textOutlineColor
                : MarkupColor::White();
            borderElem.thickness = element.thickness + element.outlineThickness * 2.0f;
            borderElem.lineStyle = LineStyle::Solid;
            renderLineWithEndpointCaps(canvas, borderElem, sCap, eCap);
        }
        renderLineWithEndpointCaps(canvas, element, sCap, eCap);
    }
};

class EllipseHandler : public DefaultBoxHandler {
public:
    void render(cv::Mat& canvas, const MarkupElement& element) const override {
        auto color = element.color.toCvScalar();
        int thick = (std::max)(1, static_cast<int>(element.thickness));
        cv::Point center((element.startPt.x + element.endPt.x) / 2,
                         (element.startPt.y + element.endPt.y) / 2);
        cv::Size axes(std::abs(element.endPt.x - element.startPt.x) / 2,
                      std::abs(element.endPt.y - element.startPt.y) / 2);
        if (axes.width > 0 && axes.height > 0) {
            if (element.fill) {
                cv::ellipse(canvas, center, axes, 0, 0, 360, color, cv::FILLED, cv::LINE_AA);
            }
            const bool customBorder = element.fill && (element.hasCustomOutline || !element.textOutlineColor.isAuto());
            const float outW = customBorder
                ? (element.hasCustomOutline ? element.outlineThickness : element.thickness)
                : static_cast<float>(thick);
            if (!element.fill || outW > 0.05f) {
                const int borderThick = (std::max)(1, static_cast<int>(std::lround(outW)));
                const cv::Scalar borderCol = (customBorder && !element.textOutlineColor.isAuto())
                    ? element.textOutlineColor.toCvScalar()
                    : color;
                if (element.lineStyle == LineStyle::Solid) {
                    cv::ellipse(canvas, center, axes, 0, 0, 360, borderCol, borderThick, cv::LINE_AA);
                } else {
                    const int numPts = 72;
                    std::vector<cv::Point> pts;
                    pts.reserve(numPts);
                    for (int i = 0; i < numPts; ++i) {
                        float theta = (2.0f * static_cast<float>(CV_PI) * i) / numPts;
                        pts.emplace_back(center.x + static_cast<int>(std::round(axes.width * std::cos(theta))),
                                         center.y + static_cast<int>(std::round(axes.height * std::sin(theta))));
                    }
                    drawPatternedPolyline(canvas, pts, true, borderCol, borderThick, element.lineStyle);
                }
            }
        }
    }

    HitArea hitTest(const MarkupElement& element, cv::Point pt, int padding) const override {
        if (element.isActive) {
            HitArea handleHit = hitTestHandles(element, pt, padding);
            if (handleHit != HitArea::None) return handleHit;
        }
        cv::Rect bbox = getBoundingBox(element);
        double rx = bbox.width * 0.5;
        double ry = bbox.height * 0.5;
        if (rx < 1e-4 || ry < 1e-4) return HitArea::None;
        double cx = bbox.x + rx;
        double cy = bbox.y + ry;
        double nx = (pt.x - cx) / rx;
        double ny = (pt.y - cy) / ry;
        double distNorm = std::sqrt(nx * nx + ny * ny);

        int p = (std::max)(padding, 6);
        double minR = (std::min)(rx, ry);
        double tol = p / minR;

        if (element.fill) {
            if (distNorm <= 1.0 + tol) return HitArea::Body;
            return HitArea::None;
        } else {
            if (std::abs(distNorm - 1.0) <= tol) return HitArea::Body;
            return HitArea::None;
        }
    }
};

class PenHandler : public DefaultBoxHandler {
public:
    cv::Rect getBoundingBox(const MarkupElement& element) const override {
        if (element.penPoints.empty()) return cv::Rect();
        int minX = element.penPoints[0].x, minY = element.penPoints[0].y;
        int maxX = minX, maxY = minY;
        for (const auto& pt : element.penPoints) {
            minX = (std::min)(minX, pt.x); minY = (std::min)(minY, pt.y);
            maxX = (std::max)(maxX, pt.x); maxY = (std::max)(maxY, pt.y);
        }
        int w = (std::max)(HandleGeometry::kDefaultMinBoxSize, maxX - minX);
        int h = (std::max)(HandleGeometry::kDefaultMinBoxSize, maxY - minY);
        return cv::Rect(minX, minY, w, h);
    }

    void render(cv::Mat& canvas, const MarkupElement& element) const override {
        auto color = element.color.toCvScalar();
        int thick = (std::max)(1, static_cast<int>(element.thickness));
        if (element.penPoints.size() >= 2) {
            bool needsSpline = false;
            if (element.smoothCurve && element.penPoints.size() >= 3) {
                double totalSegLen = 0.0;
                for (size_t i = 1; i < element.penPoints.size(); ++i) {
                    totalSegLen += std::hypot(element.penPoints[i].x - element.penPoints[i - 1].x,
                                              element.penPoints[i].y - element.penPoints[i - 1].y);
                }
                const double avgSegLen = totalSegLen / static_cast<double>(element.penPoints.size() - 1);
                needsSpline = (avgSegLen > 4.5);
            }
            const auto pts = needsSpline
                ? generateSmoothSpline(element.penPoints, 8)
                : element.penPoints;
            drawPatternedPolyline(canvas, pts, false, color, thick, element.lineStyle);
        }
    }

    HitArea hitTest(const MarkupElement& element, cv::Point pt, int padding) const override {
        if (element.penPoints.empty()) return HitArea::None;
        if (element.isActive) {
            HitArea handleHit = hitTestHandles(element, pt, padding);
            if (handleHit != HitArea::None) return handleHit;
        }
        int p = (std::max)(padding, static_cast<int>(element.thickness * 0.5f) + 6);
        double pSq = static_cast<double>(p * p);
        if (element.penPoints.size() == 1) {
            double dSq = static_cast<double>((pt.x - element.penPoints[0].x) * (pt.x - element.penPoints[0].x) +
                                             (pt.y - element.penPoints[0].y) * (pt.y - element.penPoints[0].y));
            return (dSq <= pSq) ? HitArea::Body : HitArea::None;
        }
        for (size_t i = 1; i < element.penPoints.size(); ++i) {
            const auto& p1 = element.penPoints[i - 1];
            const auto& p2 = element.penPoints[i];
            double dx = p2.x - p1.x;
            double dy = p2.y - p1.y;
            double lenSq = dx * dx + dy * dy;
            if (lenSq > 1e-6) {
                double t = std::clamp(((pt.x - p1.x) * dx + (pt.y - p1.y) * dy) / lenSq, 0.0, 1.0);
                double px = p1.x + t * dx;
                double py = p1.y + t * dy;
                double distSq = (pt.x - px) * (pt.x - px) + (pt.y - py) * (pt.y - py);
                if (distSq <= pSq) return HitArea::Body;
            }
        }
        return HitArea::None;
    }

    void resize(MarkupElement& element, int dx, int dy, HitArea handle) const override {
        if (element.penPoints.empty()) return;
        cv::Rect bbox = getBoundingBox(element);
        if (bbox.width <= 0 || bbox.height <= 0) return;

        cv::Rect newBox = HandleGeometry::computeResizedRect(bbox, dx, dy, handle, 4, 4);

        int minX = element.penPoints[0].x, minY = element.penPoints[0].y;
        int maxX = minX, maxY = minY;
        for (const auto& pt : element.penPoints) {
            minX = (std::min)(minX, pt.x); minY = (std::min)(minY, pt.y);
            maxX = (std::max)(maxX, pt.x); maxY = (std::max)(maxY, pt.y);
        }
        int origSpanX = maxX - minX;
        int origSpanY = maxY - minY;

        float scaleX = (origSpanX > 0) ? (static_cast<float>(newBox.width) / static_cast<float>(origSpanX)) : 1.0f;
        float scaleY = (origSpanY > 0) ? (static_cast<float>(newBox.height) / static_cast<float>(origSpanY)) : 1.0f;

        for (auto& pt : element.penPoints) {
            if (origSpanX > 0) {
                pt.x = newBox.x + static_cast<int>(std::round((pt.x - minX) * scaleX));
            } else {
                pt.x = newBox.x + newBox.width / 2;
            }
            if (origSpanY > 0) {
                pt.y = newBox.y + static_cast<int>(std::round((pt.y - minY) * scaleY));
            } else {
                pt.y = newBox.y + newBox.height / 2;
            }
        }
    }
};

class HighlightHandler : public DefaultBoxHandler {
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
            const uchar hr = element.color.r;
            const uchar hg = element.color.g;
            const uchar hb = element.color.b;
            const float alpha = (element.color.a > 0) ? (element.color.a / 255.0f) : 0.85f;

            for (int r = 0; r < roi.rows; ++r) {
                if (roi.channels() == 3) {
                    cv::Vec3b* ptr = roi.ptr<cv::Vec3b>(r);
                    for (int c = 0; c < roi.cols; ++c) {
                        cv::Vec3b& px = ptr[c];
                        int mb = (px[0] * hb) / 255;
                        int mg = (px[1] * hg) / 255;
                        int mr = (px[2] * hr) / 255;
                        px[0] = static_cast<uchar>(px[0] * (1.0f - alpha) + mb * alpha);
                        px[1] = static_cast<uchar>(px[1] * (1.0f - alpha) + mg * alpha);
                        px[2] = static_cast<uchar>(px[2] * (1.0f - alpha) + mr * alpha);
                    }
                } else if (roi.channels() == 4) {
                    cv::Vec4b* ptr = roi.ptr<cv::Vec4b>(r);
                    for (int c = 0; c < roi.cols; ++c) {
                        cv::Vec4b& px = ptr[c];
                        int mb = (px[0] * hb) / 255;
                        int mg = (px[1] * hg) / 255;
                        int mr = (px[2] * hr) / 255;
                        px[0] = static_cast<uchar>(px[0] * (1.0f - alpha) + mb * alpha);
                        px[1] = static_cast<uchar>(px[1] * (1.0f - alpha) + mg * alpha);
                        px[2] = static_cast<uchar>(px[2] * (1.0f - alpha) + mr * alpha);
                    }
                }
            }
        }
    }
};

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
    cv::Rect getBoundingBox(const MarkupElement& element) const override {
        if (element.textRenderSize.width <= 0 || element.textRenderSize.height <= 0) {
            std::wstring wtext = utf8ToWide(element.text);
            const_cast<MarkupElement&>(element).textRenderSize =
                measureSnipasteText(wtext, static_cast<int>(element.fontSize), element.isEditing || element.isActive);
        }
        int bw = element.textRenderSize.width;
        int bh = element.textRenderSize.height;
        if (element.text.empty()) {
            if (bw < 140) bw = static_cast<int>((std::max)(140.0f, element.fontSize * 5.0f));
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
        cv::Rect oldBox = getBoundingBox(element);
        int oldW = oldBox.width;
        int oldH = oldBox.height;

        int delta = HandleGeometry::computeScalarDelta(dx, dy, handle);
        element.fontSize += delta * 0.35f;
        if (element.fontSize < 12.0f) element.fontSize = 12.0f;
        if (element.fontSize > 180.0f) element.fontSize = 180.0f;
        element.textRenderSize = cv::Size(0, 0);

        cv::Rect newBox = getBoundingBox(element);
        int newW = newBox.width;
        int newH = newBox.height;
        int dW = newW - oldW;
        int dH = newH - oldH;

        element.startPt = HandleGeometry::computeAnchoredOrigin(element.startPt, dW, dH, handle);
        element.endPt = cv::Point(element.startPt.x + newW, element.startPt.y + newH);
    }

    void renderActiveHandles(cv::Mat& canvas, const MarkupElement& element) const override {
        if (element.isEditing) {
            // 编辑态已绘制高颜值亮蓝聚焦虚线框，此处仅点缀 8 个小巧字号缩放手柄，避免双层框线重叠杂乱
            const cv::Rect bbox = getBoundingBox(element);
            const cv::Scalar activeColor(246, 130, 59);
            const auto handles = HandleGeometry::getBoxHandles(bbox);
            for (const auto& hp : handles) {
                const cv::Rect hrect(hp.point.x - 4, hp.point.y - 4, 8, 8);
                cv::rectangle(canvas, hrect, cv::Scalar(255, 255, 255), cv::FILLED, cv::LINE_AA);
                cv::rectangle(canvas, hrect, activeColor, 1, cv::LINE_AA);
            }
            return;
        }
        DefaultBoxHandler::renderActiveHandles(canvas, element);
    }

    void render(cv::Mat& canvas, const MarkupElement& element) const override {
        auto color = element.color.toCvScalar();
        cv::Size renderedSize(0, 0);
        const bool effOutline = element.textOutline && (!element.hasCustomOutline || element.outlineThickness > 0.05f);
        const float outlineScale = element.hasCustomOutline ? (element.outlineThickness / 2.0f) : 1.0f;
        renderSnipasteStyleText(canvas, element.text, element.startPt, color, static_cast<int>(element.fontSize),
                                element.isEditing, element.isActive, element.fill, renderedSize,
                                effOutline, element.textOutlineColor,
                                element.textCaret, element.textSelAnchor, element.textLastCaretTick,
                                outlineScale);
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
            if (element.hasLeaderArrow) {
                int hw = 8;
                cv::Rect h2(element.endPt.x - hw, element.endPt.y - hw, hw * 2, hw * 2);
                if (h2.contains(pt)) return HitArea::RB; // 箭头尖端控制柄
            }
            const cv::Point scalePt = computeScaleHandlePt(element);
            if (std::hypot(pt.x - scalePt.x, pt.y - scalePt.y) <= 7.5) {
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
            bool mirroredLeft = false;
            if (element.hasLeaderArrow) {
                const double adx = static_cast<double>(element.endPt.x - element.startPt.x);
                const double ady = static_cast<double>(element.endPt.y - element.startPt.y);
                if (adx > 0.0 && ady < 0.0) {
                    mirroredLeft = true;
                }
            }
            const int signedDx = mirroredLeft ? -dx : dx;
            const float delta = static_cast<float>(signedDx - dy) * 0.18f;
            element.thickness = std::clamp(element.thickness + delta, 1.0f, 28.0f);
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
        const int scaleHandleR = 5;
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
        const int handleR = 6;
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

        if (g_gdiPlusToken != 0 && !wtext.empty()) {
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

}  // namespace

MarkupToolRegistry& MarkupToolRegistry::instance() {
    static MarkupToolRegistry s_instance;
    return s_instance;
}

MarkupToolRegistry::MarkupToolRegistry() {
    registerHandler(MarkupTool::Rectangle, std::make_shared<RectangleHandler>());
    registerHandler(MarkupTool::Line, std::make_shared<LineHandler>());
    registerHandler(MarkupTool::Arrow, std::make_shared<ArrowHandler>());
    registerHandler(MarkupTool::Ellipse, std::make_shared<EllipseHandler>());
    registerHandler(MarkupTool::Pen, std::make_shared<PenHandler>());
    registerHandler(MarkupTool::Highlight, std::make_shared<HighlightHandler>());
    registerHandler(MarkupTool::Mosaic, std::make_shared<MosaicHandler>());
    registerHandler(MarkupTool::Blur, std::make_shared<BlurHandler>());
    registerHandler(MarkupTool::Text, std::make_shared<TextHandler>());
    registerHandler(MarkupTool::Number, std::make_shared<NumberHandler>());
    registerHandler(MarkupTool::Magnifier, std::make_shared<MagnifierHandler>());
    registerHandler(MarkupTool::Spotlight, std::make_shared<SpotlightHandler>());
    registerHandler(MarkupTool::Watermark, std::make_shared<WatermarkHandler>());
    registerHandler(MarkupTool::Inpaint, std::make_shared<InpaintHandler>());
}

void MarkupToolRegistry::registerHandler(MarkupTool tool, std::shared_ptr<IMarkupToolHandler> handler) {
    m_handlers[tool] = std::move(handler);
}

const IMarkupToolHandler* MarkupToolRegistry::getHandler(MarkupTool tool) const {
    auto it = m_handlers.find(tool);
    if (it != m_handlers.end()) {
        return it->second.get();
    }
    return nullptr;
}

namespace {

uint64_t computeSampledRoiChecksum(const cv::Mat& canvas, const cv::Rect& sampleRect) noexcept {
    if (canvas.empty() || sampleRect.width <= 0 || sampleRect.height <= 0) return 0;
    uint64_t hash = 14695981039346656037ULL;
    const int stepY = (std::max)(1, sampleRect.height / 16);
    const int stepX = (std::max)(1, sampleRect.width / 16);
    const int ch = canvas.channels();
    for (int y = sampleRect.y; y < sampleRect.y + sampleRect.height; y += stepY) {
        const uchar* row = canvas.ptr<uchar>(y);
        for (int x = sampleRect.x; x < sampleRect.x + sampleRect.width; x += stepX) {
            const uchar* px = row + static_cast<size_t>(x) * static_cast<size_t>(ch);
            for (int c = 0; c < ch; ++c) {
                hash ^= static_cast<uint64_t>(px[c]);
                hash *= 1099511628211ULL;
            }
        }
    }
    return hash;
}

} // namespace

void MarkupEngine::renderFilterElementCached(cv::Mat& canvas, const MarkupElement& elem) const {
    const auto* handler = MarkupToolRegistry::instance().getHandler(elem.tool);
    if (!handler || canvas.empty()) return;

    const int x1 = (std::max)(0, (std::min)(elem.startPt.x, elem.endPt.x));
    const int y1 = (std::max)(0, (std::min)(elem.startPt.y, elem.endPt.y));
    const int x2 = (std::min)(canvas.cols, (std::max)(elem.startPt.x, elem.endPt.x));
    const int y2 = (std::min)(canvas.rows, (std::max)(elem.startPt.y, elem.endPt.y));
    if (x2 <= x1 || y2 <= y1) return;

    const cv::Rect dstRoi(x1, y1, x2 - x1, y2 - y1);
    const int paramVal = (elem.tool == MarkupTool::Inpaint) ? elem.inpaintRadius : elem.mosaicBlockSize;

    cv::Rect sampleRoi = dstRoi;
    if (elem.tool == MarkupTool::Inpaint) {
        const double safeRadius = std::clamp(static_cast<double>(elem.inpaintRadius), 1.0, 32.0);
        const int pad = (std::max)(16, static_cast<int>(std::ceil(safeRadius * 3.0)));
        sampleRoi = cv::Rect(x1 - pad, y1 - pad, (x2 - x1) + pad * 2, (y2 - y1) + pad * 2) &
                    cv::Rect(0, 0, canvas.cols, canvas.rows);
    }

    if (elem.id != 0) {
        const uint64_t checksum = computeSampledRoiChecksum(canvas, sampleRoi);
        auto it = m_filterRoiCache.find(elem.id);
        if (it != m_filterRoiCache.end()) {
            const auto& entry = it->second;
            if (entry.tool == elem.tool &&
                entry.dstRoi == dstRoi &&
                entry.paramValue == paramVal &&
                entry.channels == canvas.channels() &&
                entry.inputChecksum == checksum &&
                entry.resultPatch.size() == dstRoi.size() &&
                entry.resultPatch.type() == canvas.type()) {
                entry.resultPatch.copyTo(canvas(dstRoi));
                return;
            }
        }

        handler->render(canvas, elem);

        if (m_filterRoiCache.size() > 64) {
            m_filterRoiCache.clear();
        }
        FilterRoiCacheEntry updated;
        updated.tool = elem.tool;
        updated.dstRoi = dstRoi;
        updated.paramValue = paramVal;
        updated.channels = canvas.channels();
        updated.inputChecksum = checksum;
        updated.resultPatch = canvas(dstRoi).clone();
        m_filterRoiCache[elem.id] = std::move(updated);
        return;
    }

    handler->render(canvas, elem);
}

void MarkupEngine::renderSubset(cv::Mat& canvas, size_t count, bool includeActiveHandles) const {
    if (canvas.empty() || m_elements.empty() || count == 0) return;
    const size_t limit = (std::min)(count, m_elements.size());

    auto isBaseFilterTool = [](MarkupTool t) noexcept {
        return t == MarkupTool::Mosaic || t == MarkupTool::Blur || t == MarkupTool::Inpaint;
    };

    std::vector<const MarkupElement*> spotlights;
    for (size_t i = 0; i < limit; ++i) {
        const auto& elem = m_elements[i];
        if (!elem) continue;
        if (elem->tool == MarkupTool::Spotlight) {
            spotlights.push_back(elem.get());
        } else if (isBaseFilterTool(elem->tool)) {
            renderFilterElementCached(canvas, *elem);
        }
    }

    // 聚光灯联合遮罩层：多区域求亮区并集一次性弱化背景，且保证后续前景箭头/文字/序号始终明亮清晰
    if (!spotlights.empty()) {
        SpotlightHandler::renderUnified(canvas, spotlights);
    }

    auto renderWithColorOpacity = [](cv::Mat& targetCanvas, const IMarkupToolHandler* handler, const MarkupElement& element) {
        if (!handler || targetCanvas.empty()) return;
        if (element.color.a == 0 || element.color.a >= 255 ||
            element.tool == MarkupTool::Highlight ||
            element.tool == MarkupTool::Magnifier ||
            element.tool == MarkupTool::Watermark ||
            element.tool == MarkupTool::Spotlight ||
            element.tool == MarkupTool::Mosaic ||
            element.tool == MarkupTool::Blur ||
            element.tool == MarkupTool::Inpaint) {
            handler->render(targetCanvas, element);
            return;
        }
        const cv::Rect rawBox = handler->getBoundingBox(element);
        const int pad = (std::max)(48, static_cast<int>(std::ceil(element.thickness * 8.0f + element.fontSize * 1.5f)));
        const cv::Rect expanded(rawBox.x - pad, rawBox.y - pad, rawBox.width + pad * 2, rawBox.height + pad * 2);
        const cv::Rect roi = expanded & cv::Rect(0, 0, targetCanvas.cols, targetCanvas.rows);
        if (roi.width <= 0 || roi.height <= 0) {
            return;
        }
        const double alpha = std::clamp(static_cast<double>(element.color.a) / 255.0, 0.05, 1.0);
        cv::Mat dstRoi = targetCanvas(roi);
        cv::Mat layerRoi = dstRoi.clone();
        MarkupElement localElem = element;
        localElem.moveBy(-roi.x, -roi.y);
        handler->render(layerRoi, localElem);
        if (element.tool == MarkupTool::Text && localElem.textRenderSize.width > 0 && localElem.textRenderSize.height > 0) {
            const_cast<MarkupElement&>(element).textRenderSize = localElem.textRenderSize;
            const_cast<MarkupElement&>(element).endPt =
                cv::Point(element.startPt.x + localElem.textRenderSize.width,
                          element.startPt.y + localElem.textRenderSize.height);
        }
        cv::addWeighted(layerRoi, alpha, dstRoi, 1.0 - alpha, 0.0, dstRoi);
    };

    for (size_t i = 0; i < limit; ++i) {
        const auto& elem = m_elements[i];
        if (!elem || elem->tool == MarkupTool::Spotlight || isBaseFilterTool(elem->tool)) continue;
        if (const auto* handler = MarkupToolRegistry::instance().getHandler(elem->tool)) {
            renderWithColorOpacity(canvas, handler, *elem);
        }
    }

    if (includeActiveHandles) {
        for (size_t i = 0; i < limit; ++i) {
            const auto& elem = m_elements[i];
            if (elem && elem->isActive) {
                if (const auto* handler = MarkupToolRegistry::instance().getHandler(elem->tool)) {
                    handler->renderActiveHandles(canvas, *elem);
                }
            }
        }
    }
}

void MarkupEngine::renderAll(cv::Mat& canvas, bool includeActiveHandles) const {
    renderSubset(canvas, m_elements.size(), includeActiveHandles);
}

void MarkupEngine::renderElement(cv::Mat& canvas, const MarkupElement& element, bool includeActiveHandles) const {
    if (const auto* handler = MarkupToolRegistry::instance().getHandler(element.tool)) {
        if (element.color.a > 0 && element.color.a < 255 &&
            element.tool != MarkupTool::Highlight &&
            element.tool != MarkupTool::Magnifier &&
            element.tool != MarkupTool::Watermark &&
            element.tool != MarkupTool::Spotlight &&
            element.tool != MarkupTool::Mosaic &&
            element.tool != MarkupTool::Blur &&
            element.tool != MarkupTool::Inpaint) {
            const cv::Rect rawBox = handler->getBoundingBox(element);
            const int pad = (std::max)(48, static_cast<int>(std::ceil(element.thickness * 8.0f + element.fontSize * 1.5f)));
            const cv::Rect expanded(rawBox.x - pad, rawBox.y - pad, rawBox.width + pad * 2, rawBox.height + pad * 2);
            const cv::Rect roi = expanded & cv::Rect(0, 0, canvas.cols, canvas.rows);
            if (roi.width > 0 && roi.height > 0) {
                const double alpha = std::clamp(static_cast<double>(element.color.a) / 255.0, 0.05, 1.0);
                cv::Mat dstRoi = canvas(roi);
                cv::Mat layerRoi = dstRoi.clone();
                MarkupElement localElem = element;
                localElem.moveBy(-roi.x, -roi.y);
                handler->render(layerRoi, localElem);
                if (element.tool == MarkupTool::Text && localElem.textRenderSize.width > 0 && localElem.textRenderSize.height > 0) {
                    const_cast<MarkupElement&>(element).textRenderSize = localElem.textRenderSize;
                    const_cast<MarkupElement&>(element).endPt =
                        cv::Point(element.startPt.x + localElem.textRenderSize.width,
                                  element.startPt.y + localElem.textRenderSize.height);
                }
                cv::addWeighted(layerRoi, alpha, dstRoi, 1.0 - alpha, 0.0, dstRoi);
            }
        } else if (element.tool == MarkupTool::Mosaic ||
                   element.tool == MarkupTool::Blur ||
                   element.tool == MarkupTool::Inpaint) {
            renderFilterElementCached(canvas, element);
        } else {
            handler->render(canvas, element);
        }
        if (includeActiveHandles && element.isActive) {
            handler->renderActiveHandles(canvas, element);
        }
    }
}

}  // namespace tools3000::capture

