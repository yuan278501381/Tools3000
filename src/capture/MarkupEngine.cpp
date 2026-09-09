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
        
        std::vector<cv::Point> extended(pts.size() + 2);
        extended[0] = pts[0] - (pts[1] - pts[0]);
        for (size_t i = 0; i < pts.size(); ++i) extended[i+1] = pts[i];
        extended[extended.size()-1] = pts.back() + (pts.back() - pts[pts.size()-2]);

        std::vector<cv::Point> smoothPts;
        smoothPts.reserve((pts.size() - 1) * stepsPerSegment + 1);

        for (size_t i = 1; i < extended.size() - 2; ++i) {
            cv::Point2f p0 = extended[i-1], p1 = extended[i], p2 = extended[i+1], p3 = extended[i+2];
            for (int t_i = 0; t_i < (i == extended.size()-3 ? stepsPerSegment + 1 : stepsPerSegment); ++t_i) {
                float t = static_cast<float>(t_i) / stepsPerSegment;
                float t2 = t * t;
                float t3 = t2 * t;
                float x = 0.5f * ((2.0f * p1.x) + (-p0.x + p2.x) * t + (2.0f * p0.x - 5.0f * p1.x + 4.0f * p2.x - p3.x) * t2 + (-p0.x + 3.0f * p1.x - 3.0f * p2.x + p3.x) * t3);
                float y = 0.5f * ((2.0f * p1.y) + (-p0.y + p2.y) * t + (2.0f * p0.y - 5.0f * p1.y + 4.0f * p2.y - p3.y) * t2 + (-p0.y + 3.0f * p1.y - 3.0f * p2.y + p3.y) * t3);
                smoothPts.push_back(cv::Point(static_cast<int>(x), static_cast<int>(y)));
            }
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
        const int safeFontSize = (std::max)(10, fontSize);
        const auto [originX, originY] = tools3000::capture::CaptureTextEditor::computeTextOrigin(safeFontSize);
        const int padW = static_cast<int>(std::ceil(originX * 2.0f + 4.0f));
        const int padH = static_cast<int>(std::ceil(originY * 2.0f + 2.0f));
        if (g_gdiPlusToken == 0) {
            int w = text.empty() ? (124 + padW) : static_cast<int>((std::max)(40.0f, safeFontSize * 2.0f)) + padW;
            int h = static_cast<int>((std::max)(24.0f, safeFontSize * 1.3f)) + padH;
            return cv::Size((std::max)(w, 24 + padW), (std::max)(h, safeFontSize + padH));
        }

        Gdiplus::FontFamily fontFamily(L"Microsoft YaHei UI");
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
                                 size_t caretIndex = 0, size_t selAnchor = 0, uint32_t lastCaretTick = 0) {
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
            Gdiplus::FontFamily fontFamily(L"Microsoft YaHei UI");
            const Gdiplus::FontFamily* activeFamily = &fontFamily;
            Gdiplus::FontFamily fallbackFamily(L"Segoe UI");
            if (!fontFamily.IsAvailable()) {
                activeFamily = &fallbackFamily;
            }

            Gdiplus::StringFormat format(Gdiplus::StringFormat::GenericTypographic());
            format.SetAlignment(Gdiplus::StringAlignmentNear);
            format.SetLineAlignment(Gdiplus::StringAlignmentNear);
            format.SetFormatFlags(format.GetFormatFlags() | Gdiplus::StringFormatFlagsMeasureTrailingSpaces);

            const int safeFontSize = (std::max)(10, fontSize);
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
                    const float strokeWidth = tools3000::capture::CaptureTextEditor::computeOutlineStrokeWidth(safeFontSize);
                    Gdiplus::Pen strokePen(strokeColor, strokeWidth);
                    strokePen.SetLineJoin(Gdiplus::LineJoinRound);
                    g.DrawPath(&strokePen, &textPath);
                }

                // 正文矢量文字填充
                Gdiplus::SolidBrush textBrush(Gdiplus::Color(255, rVal, gVal, bVal));
                g.FillPath(&textBrush, &textPath);
            } else if (isEditing || isSelected) {
                // 空文本编辑态：绘制提示占位文字
                const std::wstring placeholder = tools3000::core::isUiLanguageChinese()
                    ? L"点击输入文字..."
                    : L"Click to enter text...";
                Gdiplus::Font placeholderFont(activeFamily, static_cast<Gdiplus::REAL>((std::max)(12.0f, safeFontSize * 0.88f)), Gdiplus::FontStyleRegular, Gdiplus::UnitPixel);
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
        Gdiplus::FontFamily primaryFamily(preferCjkFont ? L"Microsoft YaHei UI" : L"Segoe UI");
        Gdiplus::FontFamily fallbackFamily(preferCjkFont ? L"Segoe UI" : L"Microsoft YaHei UI");
        const Gdiplus::FontFamily* activeFamily = primaryFamily.IsAvailable() ? &primaryFamily : &fallbackFamily;

        const std::size_t charLen = numWStr.size();
        float emSize = 16.0f * scale;
        if (preferCjkFont) {
            emSize = ((charLen >= 3) ? 11.8f : (charLen == 2 ? 13.0f : 15.0f)) * scale;
        } else {
            emSize = ((charLen >= 4) ? 11.2f : (charLen == 3 ? 12.2f : (charLen == 2 ? 13.8f : 16.0f))) * scale;
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
        if (shape == tools3000::capture::NumberBadgeShape::RoundedSquare && pathBounds.Width > 0.0f) {
            // 圆角矩形模式下，多字符（如 VIII、十一、二十五）自动横向延展为精致微晶胶囊，绝不挤字
            boxW = (std::max)(boxH, std::ceil(pathBounds.Width + 14.0f * scale));
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

void MarkupEngine::setBaseImage(const cv::Mat& image) {
    m_baseImage = image.clone();
    m_elements.clear();
    m_undoStack.clear();
    resetAllNumbers();
    LOG_DEBUG("标注引擎: 设置底图 {}x{}", image.cols, image.rows);
}

void MarkupEngine::updateBaseImage(const cv::Mat& image) {
    // 仅替换底图，保留标注/撤销栈/序号状态（选区二次调整后重裁用）
    m_baseImage = image.clone();
}

void MarkupEngine::translateAll(int dx, int dy) {
    if (dx == 0 && dy == 0) return;
    for (auto& e : m_elements) e->moveBy(dx, dy);
    for (auto& e : m_undoStack) e->moveBy(dx, dy);  // 撤销栈一并平移，保证 redo 位置正确
}

cv::Mat MarkupEngine::getCompositeImage(bool includeActiveHandles) const {
    if (m_baseImage.empty()) return {};

    cv::Mat result = m_baseImage.clone();
    renderAll(result, includeActiveHandles);
    return result;
}

// ─────────────────────────────────────────────────────────────────────────────
// 元素操作
// ─────────────────────────────────────────────────────────────────────────────

MarkupElement* MarkupEngine::addElement(std::unique_ptr<MarkupElement> element) {
    m_undoStack.clear();  // 新增元素后清空重做栈
    element->id = g_elementIdCounter++;
    m_elements.push_back(std::move(element));
    return m_elements.back().get();
}

bool MarkupEngine::undo() {
    if (m_elements.empty()) return false;

    m_undoStack.push_back(std::move(m_elements.back()));
    m_elements.pop_back();

    // 重新按各序号制式独立校准计数器
    recalculateNextNumbers();

    LOG_DEBUG("标注引擎: 撤销, 剩余元素数={}", m_elements.size());
    return true;
}

bool MarkupEngine::redo() {
    if (m_undoStack.empty()) return false;

    auto restored = std::move(m_undoStack.back());
    if (restored->tool == MarkupTool::Number) {
        const size_t slot = numberFormatSlot(restored->numberFormat);
        m_nextNumbers[slot] = (std::max)(m_nextNumbers[slot], restored->numberValue + 1);
    }
    m_elements.push_back(std::move(restored));
    m_undoStack.pop_back();
    LOG_DEBUG("标注引擎: 重做, 剩余元素数={}", m_elements.size());
    return true;
}

void MarkupEngine::clearAll() {
    m_elements.clear();
    m_undoStack.clear();
    resetAllNumbers();
}

void MarkupEngine::removeElement(uint32_t id) {
    m_elements.erase(std::remove_if(m_elements.begin(), m_elements.end(),
        [id](const std::unique_ptr<MarkupElement>& e) { return e->id == id; }),
        m_elements.end());

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
    auto elem = std::make_unique<MarkupElement>();
    elem->tool = MarkupTool::Number;
    elem->startPt = position;
    elem->endPt = position;
    elem->color = color;
    elem->numberValue = m_nextNumbers[numberFormatSlot(format)]++;
    elem->dpiScale = dpiScale;
    elem->numberShape = shape;
    elem->numberFormat = format;
    return addElement(std::move(elem));
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

    std::vector<float> pattern;
    if (style == LineStyle::Dashed) {
        pattern = { 10.0f, 6.0f }; // 10px 实线, 6px 空白
    } else if (style == LineStyle::Dotted) {
        pattern = { 3.0f, 4.0f };  // 3px 实线点, 4px 空白
    } else if (style == LineStyle::DashDot) {
        pattern = { 10.0f, 4.0f, 3.0f, 4.0f }; // 线 - 间隙 - 点 - 间隙
    } else {
        pattern = { 1000.0f, 0.0f };
    }

    size_t patIdx = 0;
    float patRem = pattern[0];
    bool isDraw = true;

    size_t numEdges = closed ? points.size() : points.size() - 1;
    for (size_t i = 0; i < numEdges; ++i) {
        cv::Point ptA = points[i];
        cv::Point ptB = points[(i + 1) % points.size()];

        float dx = static_cast<float>(ptB.x - ptA.x);
        float dy = static_cast<float>(ptB.y - ptA.y);
        float dist = std::hypot(dx, dy);
        if (dist < 1e-4f) continue;

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
                cv::line(canvas, pStart, pEnd, color, thick, cv::LINE_AA);
            }

            edgeTraveled = nextTraveled;
            patRem -= canAdvance;
            if (patRem <= 1e-4f) {
                patIdx = (patIdx + 1) % pattern.size();
                patRem = pattern[patIdx];
                isDraw = (patIdx % 2 == 0);
            }
        }
    }
}

void drawPatternedLine(cv::Mat& canvas, cv::Point p1, cv::Point p2, const cv::Scalar& color, int thick, LineStyle style) {
    if (style == LineStyle::Solid) {
        cv::line(canvas, p1, p2, color, thick, cv::LINE_AA);
    } else {
        drawPatternedPolyline(canvas, { p1, p2 }, false, color, thick, style);
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

class LineHandler : public IMarkupToolHandler {
public:
    cv::Rect getBoundingBox(const MarkupElement& element) const override {
        int x1 = (std::min)(element.startPt.x, element.endPt.x);
        int y1 = (std::min)(element.startPt.y, element.endPt.y);
        int x2 = (std::max)(element.startPt.x, element.endPt.x);
        int y2 = (std::max)(element.startPt.y, element.endPt.y);
        int pad = (std::max)(1, static_cast<int>(std::ceil(element.thickness / 2.0f)));
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
        }
        double dx = element.endPt.x - element.startPt.x;
        double dy = element.endPt.y - element.startPt.y;
        double lenSq = dx * dx + dy * dy;
        double dist = 0.0;
        if (lenSq < 1e-6) {
            dist = std::hypot(pt.x - element.startPt.x, pt.y - element.startPt.y);
        } else {
            double t = std::clamp(((pt.x - element.startPt.x) * dx + (pt.y - element.startPt.y) * dy) / lenSq, 0.0, 1.0);
            double px = element.startPt.x + t * dx;
            double py = element.startPt.y + t * dy;
            dist = std::hypot(pt.x - px, pt.y - py);
        }
        double tol = (std::max)(static_cast<double>(padding), static_cast<double>(element.thickness) * 0.5 + 6.0);
        if (dist <= tol) return HitArea::Body;
        return HitArea::None;
    }

    void resize(MarkupElement& element, int dx, int dy, HitArea handle) const override {
        switch (handle) {
            case HitArea::LT:
            case HitArea::T:
            case HitArea::L:
            case HitArea::LB:
                element.startPt.x += dx;
                element.startPt.y += dy;
                break;
            case HitArea::RB:
            case HitArea::B:
            case HitArea::R:
            case HitArea::RT:
                element.endPt.x += dx;
                element.endPt.y += dy;
                break;
            default: break;
        }
    }

    void render(cv::Mat& canvas, const MarkupElement& element) const override {
        auto color = element.color.toCvScalar();
        int thick = (std::max)(1, static_cast<int>(element.thickness));
        drawPatternedLine(canvas, element.startPt, element.endPt, color, thick, element.lineStyle);
    }

    void renderActiveHandles(cv::Mat& canvas, const MarkupElement& element) const override {
        cv::Scalar activeColor(255, 140, 0); // Azure/Blue
        drawPatternedLine(canvas, element.startPt, element.endPt, activeColor, 1, LineStyle::Dashed);
        int r = 5;
        cv::Point handles[2] = { element.startPt, element.endPt };
        for (const auto& pt : handles) {
            cv::circle(canvas, pt, r + 1, cv::Scalar(255, 255, 255), cv::FILLED, cv::LINE_AA);
            cv::circle(canvas, pt, r + 1, activeColor, 1, cv::LINE_AA);
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
        int pad = (std::max)(14, static_cast<int>(std::ceil(element.thickness * 2.5f)));
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
        }

        double dx = element.endPt.x - element.startPt.x;
        double dy = element.endPt.y - element.startPt.y;
        double lenSq = dx * dx + dy * dy;
        double dist = 0.0;
        if (lenSq < 1e-6) {
            dist = std::hypot(pt.x - element.startPt.x, pt.y - element.startPt.y);
        } else {
            double t = std::clamp(((pt.x - element.startPt.x) * dx + (pt.y - element.startPt.y) * dy) / lenSq, 0.0, 1.0);
            double px = element.startPt.x + t * dx;
            double py = element.startPt.y + t * dy;
            dist = std::hypot(pt.x - px, pt.y - py);
        }

        double headLen = (std::max)(14.0, static_cast<double>(element.thickness) * 3.5);
        double distTip = std::hypot(pt.x - element.endPt.x, pt.y - element.endPt.y);
        if (distTip <= headLen) {
            return HitArea::Body;
        }
        if (element.arrowStyle == ArrowStyle::DoubleEnded) {
            double distStart = std::hypot(pt.x - element.startPt.x, pt.y - element.startPt.y);
            if (distStart <= headLen) {
                return HitArea::Body;
            }
        }

        double tol = (std::max)(static_cast<double>(padding), static_cast<double>(element.thickness) * 0.5 + 8.0);
        if (dist <= tol) return HitArea::Body;
        return HitArea::None;
    }

    void resize(MarkupElement& element, int dx, int dy, HitArea handle) const override {
        switch (handle) {
            case HitArea::LT:
            case HitArea::T:
            case HitArea::L:
            case HitArea::LB:
                element.startPt.x += dx;
                element.startPt.y += dy;
                break;
            case HitArea::RB:
            case HitArea::B:
            case HitArea::R:
            case HitArea::RT:
                element.endPt.x += dx;
                element.endPt.y += dy;
                break;
            default: break;
        }
    }

    void renderActiveHandles(cv::Mat& canvas, const MarkupElement& element) const override {
        cv::Scalar activeColor(255, 140, 0); // Azure/Blue
        drawPatternedLine(canvas, element.startPt, element.endPt, activeColor, 1, LineStyle::Dashed);
        int r = 5;
        cv::Point handles[2] = { element.startPt, element.endPt };
        for (const auto& pt : handles) {
            cv::circle(canvas, pt, r + 1, cv::Scalar(255, 255, 255), cv::FILLED, cv::LINE_AA);
            cv::circle(canvas, pt, r + 1, activeColor, 1, cv::LINE_AA);
        }
    }

    void render(cv::Mat& canvas, const MarkupElement& element) const override {
        auto color = element.color.toCvScalar();
        int thick = (std::max)(1, static_cast<int>(element.thickness));

        float dx = static_cast<float>(element.endPt.x - element.startPt.x);
        float dy = static_cast<float>(element.endPt.y - element.startPt.y);
        float dist = std::hypot(dx, dy);
        if (dist < 2.0f) return;

        double angle = std::atan2(dy, dx);
        double headLen = (std::max)(12.0, thick * 3.5);
        if (headLen > dist * 0.45) headLen = dist * 0.45;

        auto drawHeadAt = [&](cv::Point tip, double a, ArrowStyle style, cv::Point& shaftEnd) {
            double cA = std::cos(a);
            double sA = std::sin(a);
            double nX = -sA;
            double nY = cA;

            if (style == ArrowStyle::Thin) {
                // 极简细线开放角 Thin Open Chevron
                double wingAngle = CV_PI / 6.0; // 30 deg
                cv::Point w1(static_cast<int>(std::round(tip.x - headLen * std::cos(a - wingAngle))),
                             static_cast<int>(std::round(tip.y - headLen * std::sin(a - wingAngle))));
                cv::Point w2(static_cast<int>(std::round(tip.x - headLen * std::cos(a + wingAngle))),
                             static_cast<int>(std::round(tip.y - headLen * std::sin(a + wingAngle))));
                cv::line(canvas, tip, w1, color, thick, cv::LINE_AA);
                cv::line(canvas, tip, w2, color, thick, cv::LINE_AA);
                shaftEnd = tip;
            } else if (style == ArrowStyle::Tech) {
                // 圆润科技角 Modern Swept Aerodynamic Wing
                double halfW = headLen * 0.55;
                cv::Point w1(static_cast<int>(std::round(tip.x - headLen * cA + halfW * nX)),
                             static_cast<int>(std::round(tip.y - headLen * sA + halfW * nY)));
                cv::Point w2(static_cast<int>(std::round(tip.x - headLen * cA - halfW * nX)),
                             static_cast<int>(std::round(tip.y - headLen * sA - halfW * nY)));
                cv::Point notch(static_cast<int>(std::round(tip.x - headLen * 0.65 * cA)),
                                static_cast<int>(std::round(tip.y - headLen * 0.65 * sA)));
                std::vector<std::vector<cv::Point>> headPoly = { { tip, w1, notch, w2 } };
                cv::fillPoly(canvas, headPoly, color, cv::LINE_AA);
                shaftEnd = notch;
            } else {
                // 经典实心尖角 Classic Filled Triangle (Standard)
                double halfW = headLen * 0.45;
                cv::Point basePt(static_cast<int>(std::round(tip.x - headLen * cA)),
                                 static_cast<int>(std::round(tip.y - headLen * sA)));
                cv::Point w1(static_cast<int>(std::round(basePt.x + halfW * nX)),
                             static_cast<int>(std::round(basePt.y + halfW * nY)));
                cv::Point w2(static_cast<int>(std::round(basePt.x - halfW * nX)),
                             static_cast<int>(std::round(basePt.y - halfW * nY)));
                std::vector<std::vector<cv::Point>> headPoly = { { tip, w1, w2 } };
                cv::fillPoly(canvas, headPoly, color, cv::LINE_AA);
                shaftEnd = basePt;
            }
        };

        if (element.arrowStyle == ArrowStyle::DoubleEnded) {
            cv::Point shaftStart = element.startPt;
            cv::Point shaftEnd = element.endPt;
            drawHeadAt(element.endPt, angle, ArrowStyle::Standard, shaftEnd);
            drawHeadAt(element.startPt, angle + CV_PI, ArrowStyle::Standard, shaftStart);
            drawPatternedLine(canvas, shaftStart, shaftEnd, color, thick, element.lineStyle);
        } else {
            cv::Point shaftEnd = element.endPt;
            drawHeadAt(element.endPt, angle, element.arrowStyle, shaftEnd);
            drawPatternedLine(canvas, element.startPt, shaftEnd, color, thick, element.lineStyle);
        }
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
            if (element.lineStyle == LineStyle::Solid) {
                cv::ellipse(canvas, center, axes, 0, 0, 360, color, thick, cv::LINE_AA);
            } else {
                const int numPts = 72;
                std::vector<cv::Point> pts;
                pts.reserve(numPts);
                for (int i = 0; i < numPts; ++i) {
                    float theta = (2.0f * static_cast<float>(CV_PI) * i) / numPts;
                    pts.emplace_back(center.x + static_cast<int>(std::round(axes.width * std::cos(theta))),
                                     center.y + static_cast<int>(std::round(axes.height * std::sin(theta))));
                }
                drawPatternedPolyline(canvas, pts, true, color, thick, element.lineStyle);
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
            if (element.lineStyle == LineStyle::Solid) {
                cv::polylines(canvas, element.penPoints, false, color, thick, cv::LINE_AA);
            } else {
                for (size_t i = 1; i < element.penPoints.size(); ++i) {
                    drawPatternedLine(canvas, element.penPoints[i - 1], element.penPoints[i], color, thick, element.lineStyle);
                }
            }
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

        int bs = element.mosaicBlockSize;
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
        renderSnipasteStyleText(canvas, element.text, element.startPt, color, static_cast<int>(element.fontSize),
                                element.isEditing, element.isActive, element.fill, renderedSize,
                                element.textOutline, element.textOutlineColor,
                                element.textCaret, element.textSelAnchor, element.textLastCaretTick);
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
        const double r = static_cast<double>(element.magnifierRadius);
        // 同心模式下将“引出靶心手柄”置于透镜右上方约 -55° 弧线处，完全避开 T 与 RT 缩放手柄
        constexpr double kAngle = -55.0 * 3.14159265358979323846 / 180.0;
        return cv::Point(
            static_cast<int>(std::lround(lc.x + r * std::cos(kAngle))),
            static_cast<int>(std::lround(lc.y + r * std::sin(kAngle))));
    }

    cv::Rect getBoundingBox(const MarkupElement& element) const override {
        const cv::Point lc = resolveLensCenter(element);
        return cv::Rect(lc.x - element.magnifierRadius, lc.y - element.magnifierRadius,
                        element.magnifierRadius * 2, element.magnifierRadius * 2);
    }

    HitArea hitTest(const MarkupElement& element, cv::Point pt, int padding) const override {
        const cv::Rect lensBox = getBoundingBox(element);
        const cv::Point lc = resolveLensCenter(element);
        const int r = element.magnifierRadius;
        const float zoom = std::clamp(element.magnifierScale, 1.1f, 8.0f);
        const int srcRingR = std::clamp(static_cast<int>(std::lround(r / zoom)), 8, std::max(10, r * 2 / 3));

        if (element.isActive) {
            const float dpi = (std::max)(1.0f, element.dpiScale);
            const cv::Point focusPt = computeFocusHandlePoint(element);
            const bool offsetMode = isOffsetCallout(element);
            const int focusHitR = offsetMode
                ? (std::max)(static_cast<int>(std::lround(12.0f * dpi)), srcRingR + static_cast<int>(std::lround(5.0f * dpi)))
                : static_cast<int>(std::lround(11.0f * dpi));
            if (std::hypot(static_cast<double>(pt.x - focusPt.x),
                           static_cast<double>(pt.y - focusPt.y)) <= focusHitR) {
                // 同心模式下拖拽右上弧线青色引出手柄：保持源目标 startPt 在原位不动，将大透镜 endPt 引出至空白处！
                // 已分离引出模式下拖拽源取景小环：独立移动采样源 startPt，大透镜留在空白处不动！
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
            if (std::hypot(static_cast<double>(pt.x - element.startPt.x),
                           static_cast<double>(pt.y - element.startPt.y)) <= std::max(12, srcRingR + padding)) {
                return HitArea::FocusSource;
            }
        }

        bool insideLens = false;
        if (element.numberShape == NumberBadgeShape::RoundedSquare) {
            const cv::Rect padded(lensBox.x - padding, lensBox.y - padding,
                                  lensBox.width + padding * 2, lensBox.height + padding * 2);
            insideLens = padded.contains(pt);
        } else {
            insideLens = std::hypot(static_cast<double>(pt.x - lc.x),
                                    static_cast<double>(pt.y - lc.y)) <= static_cast<double>(r + padding);
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
        const int delta = HandleGeometry::computeScalarDelta(dx, dy, handle);
        element.magnifierRadius = std::clamp(element.magnifierRadius + delta, 20, 500);
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

        // 绘制取景源靶心手柄（同心时位于右上弧线供一键拉出旁路放大，分离时位于源取景小环中心）
        const cv::Point focusPt = computeFocusHandlePoint(element);
        cv::circle(canvas, focusPt, focusOuterR, cv::Scalar(255, 255, 255), cv::FILLED, cv::LINE_AA);
        cv::circle(canvas, focusPt, focusOuterR, focusCyan, 2, cv::LINE_AA);
        cv::circle(canvas, focusPt, focusInnerR, focusCyan, cv::FILLED, cv::LINE_AA);
    }

    void render(cv::Mat& canvas, const MarkupElement& element) const override {
        const int r = element.magnifierRadius;
        const float zoom = std::clamp(element.magnifierScale, 1.1f, 8.0f);
        const int srcR = static_cast<int>(std::lround(r / zoom));
        if (r <= 4 || srcR <= 1 || canvas.empty()) return;

        const cv::Point srcCenter = element.startPt;
        const cv::Point lensCenter = resolveLensCenter(element);
        const cv::Rect screenBounds(0, 0, canvas.cols, canvas.rows);

        // 1. 先从纯净未叠加本元素阴影/引线的画布采样源区域（边缘使用夹紧延展，消除黑边与比例失真）
        const cv::Rect srcRect(srcCenter.x - srcR, srcCenter.y - srcR, srcR * 2, srcR * 2);
        const cv::Rect dstRect(lensCenter.x - r, lensCenter.y - r, r * 2, r * 2);
        const cv::Rect validDst = dstRect & screenBounds;

        cv::Mat srcPatch(srcR * 2, srcR * 2, canvas.type(), cv::Scalar::all(28));
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
        // 让放大后的小号文字笔画如刀刻般紧致清脆，同时彻底杜绝传统过度锐化产生的白边光晕 (Halo/Ringing)
        cv::Mat enlarged;
        cv::resize(srcPatch, enlarged, cv::Size(r * 2, r * 2), 0, 0, cv::INTER_LANCZOS4);
        if (zoom >= 1.25f && r >= 18) {
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
        const float cornerR = std::clamp(r * 0.28f, 8.0f * dpi, 32.0f * dpi);
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
            Gdiplus::SolidBrush haloBr(Gdiplus::Color(115, 8, 12, 22));
            Gdiplus::SolidBrush mainBr(Gdiplus::Color(250, element.color.r, element.color.g, element.color.b));
            Gdiplus::SolidBrush coreBr(rimLum > 0.72f
                                           ? Gdiplus::Color(235, 30, 41, 59)
                                           : Gdiplus::Color(248, 255, 255, 255));
            const float hr = jewelR + 1.1f * sqrtDpi;
            const float cr = std::max(1.0f, jewelR * 0.44f);
            g.FillEllipse(&haloBr, ax - hr, ay - hr + 0.5f * sqrtDpi, hr * 2.0f, hr * 2.0f);
            g.FillEllipse(&mainBr, ax - jewelR, ay - jewelR, jewelR * 2.0f, jewelR * 2.0f);
            g.FillEllipse(&coreBr, ax - cr, ay - cr, cr * 2.0f, cr * 2.0f);
        };

        // 2. 引出式详图模式 (Offset Callout Loupe)：
        // 采用 Apple Keynote / CleanShot X 精密单轴渐变引线 + 双端同心微宝石锚点 + 纤细三明治源取景环（零面积遮挡正文）
        const double calloutDist = std::hypot(static_cast<double>(lensCenter.x - srcCenter.x),
                                              static_cast<double>(lensCenter.y - srcCenter.y));
        const int srcRingR = std::clamp(srcR, 8, std::max(10, (r * 2) / 3));
        const float srcCornerR = std::max(4.0f * sqrtDpi, srcRingR * 0.28f);
        bool hasLeaderBridge = false;
        float leaderUx = 0.0f;
        float leaderUy = 0.0f;
        float lensEdgeDist = static_cast<float>(r);

        if (calloutDist > 8.0) {
            const int padCallout = std::max(14, static_cast<int>(std::ceil(16.0f * sqrtDpi)));
            const int minX = std::max(0, std::min(srcCenter.x - srcRingR, lensCenter.x - r) - padCallout);
            const int minY = std::max(0, std::min(srcCenter.y - srcRingR, lensCenter.y - r) - padCallout);
            const int maxX = std::min(canvas.cols, std::max(srcCenter.x + srcRingR, lensCenter.x + r) + padCallout);
            const int maxY = std::min(canvas.rows, std::max(srcCenter.y + srcRingR, lensCenter.y + r) + padCallout);
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
                    const float sr = static_cast<float>(srcRingR);
                    const float lr = static_cast<float>(r);

                    // 2.1 纤细三明治源取景定位小环（外微影 + 1.45px 主色细环 + 0.75px 内侧高反差微衬，内部 100% 透明原图）
                    Gdiplus::Pen srcShadowPen(Gdiplus::Color(115, 8, 12, 22), 2.5f * sqrtDpi);
                    Gdiplus::Pen srcMainPen(Gdiplus::Color(242, element.color.r, element.color.g, element.color.b),
                                            1.45f * sqrtDpi);
                    Gdiplus::Pen srcInnerPen(rimLum > 0.72f
                                                 ? Gdiplus::Color(85, 15, 23, 42)
                                                 : Gdiplus::Color(135, 255, 255, 255),
                                             0.8f * sqrtDpi);
                    const float srInner = std::max(3.0f, sr - 1.0f * sqrtDpi);
                    if (isRoundedSquare) {
                        Gdiplus::GraphicsPath sp, spIn;
                        buildRoundedPath(sp, sx - sr, sy - sr, sr * 2.0f, sr * 2.0f, srcCornerR);
                        buildRoundedPath(spIn, sx - srInner, sy - srInner, srInner * 2.0f, srInner * 2.0f,
                                         std::max(2.0f, srcCornerR - 1.0f * sqrtDpi));
                        g.DrawPath(&srcShadowPen, &sp);
                        g.DrawPath(&srcMainPen, &sp);
                        g.DrawPath(&srcInnerPen, &spIn);
                    } else {
                        g.DrawEllipse(&srcShadowPen, sx - sr, sy - sr, sr * 2.0f, sr * 2.0f);
                        g.DrawEllipse(&srcMainPen, sx - sr, sy - sr, sr * 2.0f, sr * 2.0f);
                        g.DrawEllipse(&srcInnerPen, sx - srInner, sy - srInner, srInner * 2.0f, srInner * 2.0f);
                    }

                    // 2.2 单轴流光渐变精密引线 + 源端微宝石铆接点
                    const float dx = lx - sx;
                    const float dy = ly - sy;
                    const float d = std::hypot(dx, dy);
                    if (d > 1.0f) {
                        leaderUx = dx / d;
                        leaderUy = dy / d;
                        const float srcEdgeDist = computeLoupeBoundaryDistance(
                            leaderUx, leaderUy, sr, srcCornerR, isRoundedSquare);
                        lensEdgeDist = computeLoupeBoundaryDistance(
                            -leaderUx, -leaderUy, lr, cornerR, isRoundedSquare);
                        if (d > srcEdgeDist + lensEdgeDist + 4.0f * sqrtDpi) {
                            hasLeaderBridge = true;
                            const Gdiplus::PointF pSrc(sx + leaderUx * srcEdgeDist, sy + leaderUy * srcEdgeDist);
                            const Gdiplus::PointF pLens(lx - leaderUx * lensEdgeDist, ly - leaderUy * lensEdgeDist);

                            Gdiplus::Pen shadowBridgePen(Gdiplus::Color(110, 8, 12, 22), 2.8f * sqrtDpi);
                            shadowBridgePen.SetStartCap(Gdiplus::LineCapRound);
                            shadowBridgePen.SetEndCap(Gdiplus::LineCapRound);
                            g.DrawLine(&shadowBridgePen, pSrc, pLens);

                            Gdiplus::LinearGradientBrush bridgeGrad(
                                pSrc, pLens,
                                Gdiplus::Color(168, element.color.r, element.color.g, element.color.b),
                                Gdiplus::Color(250, element.color.r, element.color.g, element.color.b));
                            Gdiplus::Pen mainBridgePen(&bridgeGrad, 1.65f * sqrtDpi);
                            mainBridgePen.SetStartCap(Gdiplus::LineCapRound);
                            mainBridgePen.SetEndCap(Gdiplus::LineCapRound);
                            g.DrawLine(&mainBridgePen, pSrc, pLens);

                            // 源端三层同心微宝石铆接锚点
                            drawJewelAnchor(g, pSrc.X, pSrc.Y, 2.5f * sqrtDpi);
                        }
                    }
                }
                blendOverlay(canvas, calloutLayer, minX, minY);
            }
        }

        if (validDst.area() <= 0) return;

        // 3. 构建独立高定蓝宝石光学透镜层（10 层余弦漫射悬浮投影 + 超清画面 + 内壁景深暗角 + 弧面高光 + 三明治阳极氧化镜圈）
        const int pad = std::max(22, static_cast<int>(std::ceil(26.0f * sqrtDpi)));
        const int layerSz = r * 2 + pad * 2;
        cv::Mat lensLayer(layerSz, layerSz, CV_8UC4, cv::Scalar(0, 0, 0, 0));
        {
            Gdiplus::Bitmap bmp(layerSz, layerSz, static_cast<INT>(lensLayer.step),
                                PixelFormat32bppARGB, lensLayer.data);
            Gdiplus::Graphics g(&bmp);
            g.SetSmoothingMode(Gdiplus::SmoothingModeHighQuality);
            g.SetPixelOffsetMode(Gdiplus::PixelOffsetModeHighQuality);
            g.SetTextRenderingHint(Gdiplus::TextRenderingHintAntiAlias);

            const float cx = static_cast<float>(pad + r) + 0.5f;
            const float cy = static_cast<float>(pad + r) + 0.5f;
            const float rf = static_cast<float>(r);

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
                    buildRoundedPath(sp, cx - rf - exp, cy - rf - exp + dy,
                                     (rf + exp) * 2.0f, (rf + exp) * 2.0f, cornerR + exp);
                    g.FillPath(&sb, &sp);
                } else {
                    g.FillEllipse(&sb, cx - rf - exp, cy - rf - exp + dy,
                                  (rf + exp) * 2.0f, (rf + exp) * 2.0f);
                }
            }
        }

        // 3.2 使用浮点亚像素平滑蒙版将 Lanczos4+CAS 超清放大画面无缝嵌入 lensLayer 中心
        cv::Mat maskMat(r * 2, r * 2, CV_8UC4, cv::Scalar(0, 0, 0, 0));
        {
            Gdiplus::Bitmap mbmp(r * 2, r * 2, static_cast<INT>(maskMat.step),
                                 PixelFormat32bppARGB, maskMat.data);
            Gdiplus::Graphics mg(&mbmp);
            mg.SetSmoothingMode(Gdiplus::SmoothingModeHighQuality);
            mg.SetPixelOffsetMode(Gdiplus::PixelOffsetModeHighQuality);
            Gdiplus::SolidBrush opaqueWhite(Gdiplus::Color(255, 255, 255, 255));
            if (isRoundedSquare) {
                Gdiplus::GraphicsPath mp;
                buildRoundedPath(mp, 0.5f, 0.5f, r * 2.0f - 1.0f, r * 2.0f - 1.0f, cornerR);
                mg.FillPath(&opaqueWhite, &mp);
            } else {
                mg.FillEllipse(&opaqueWhite, 0.5f, 0.5f, r * 2.0f - 1.0f, r * 2.0f - 1.0f);
            }
        }

        cv::Mat enlargedBgra;
        if (enlarged.channels() == 3) {
            cv::cvtColor(enlarged, enlargedBgra, cv::COLOR_BGR2BGRA);
        } else {
            enlargedBgra = enlarged.clone();
        }

        for (int y = 0; y < r * 2; ++y) {
            const auto* mRow = maskMat.ptr<cv::Vec4b>(y);
            const auto* sRow = enlargedBgra.ptr<cv::Vec4b>(y);
            auto* dRow = lensLayer.ptr<cv::Vec4b>(pad + y);
            for (int x = 0; x < r * 2; ++x) {
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
            Gdiplus::Bitmap bmp(layerSz, layerSz, static_cast<INT>(lensLayer.step),
                                PixelFormat32bppARGB, lensLayer.data);
            Gdiplus::Graphics g(&bmp);
            g.SetSmoothingMode(Gdiplus::SmoothingModeHighQuality);
            g.SetPixelOffsetMode(Gdiplus::PixelOffsetModeHighQuality);
            g.SetTextRenderingHint(Gdiplus::TextRenderingHintAntiAlias);

            const float cx = static_cast<float>(pad + r) + 0.5f;
            const float cy = static_cast<float>(pad + r) + 0.5f;
            const float rf = static_cast<float>(r);

            // (a) 光学凸透镜内壁微凹景深环 (Subtle Inner Rim Vignette)，消除边缘平贴感
            if (r >= 24) {
                const float vigR1 = std::max(6.0f, rf - borderW * 0.55f - 1.2f);
                const float vigR2 = std::max(5.0f, vigR1 - 2.2f * sqrtDpi);
                Gdiplus::Pen vigPen1(Gdiplus::Color(36, 8, 12, 24), 2.4f * sqrtDpi);
                Gdiplus::Pen vigPen2(Gdiplus::Color(15, 8, 12, 24), 2.6f * sqrtDpi);
                if (isRoundedSquare) {
                    Gdiplus::GraphicsPath vp1, vp2;
                    buildRoundedPath(vp1, cx - vigR1, cy - vigR1, vigR1 * 2.0f, vigR1 * 2.0f, std::max(3.0f, cornerR - 1.5f));
                    buildRoundedPath(vp2, cx - vigR2, cy - vigR2, vigR2 * 2.0f, vigR2 * 2.0f, std::max(2.0f, cornerR - 3.5f));
                    g.DrawPath(&vigPen1, &vp1);
                    g.DrawPath(&vigPen2, &vp2);
                } else {
                    g.DrawEllipse(&vigPen1, cx - vigR1, cy - vigR1, vigR1 * 2.0f, vigR1 * 2.0f);
                    g.DrawEllipse(&vigPen2, cx - vigR2, cy - vigR2, vigR2 * 2.0f, vigR2 * 2.0f);
                }
            }

            // (b) 顶部蓝宝石冷白镀膜折射高光弧（仅点亮最顶缘月牙区，中心 90% 阅读视野 100% 纯净无遮挡）
            if (r >= 28) {
                Gdiplus::Pen glareOuterPen(Gdiplus::Color(56, 255, 255, 255), std::max(1.3f, 1.6f * sqrtDpi));
                glareOuterPen.SetStartCap(Gdiplus::LineCapRound);
                glareOuterPen.SetEndCap(Gdiplus::LineCapRound);
                if (!isRoundedSquare) {
                    const float gr = rf - borderW * 0.85f - 1.8f * sqrtDpi;
                    if (gr > 8.0f) {
                        g.DrawArc(&glareOuterPen, cx - gr, cy - gr, gr * 2.0f, gr * 2.0f, 214.0f, 112.0f);
                    }
                } else {
                    const float inset = borderW * 0.85f + 2.2f * sqrtDpi;
                    g.DrawLine(&glareOuterPen, cx - rf + cornerR * 0.9f, cy - rf + inset,
                               cx + rf - cornerR * 0.9f, cy - rf + inset);
                }
            }

            // (c) 倍率微晶角标（当 element.fill 开启且半径充裕时，在透镜底部内侧悬浮展示如 "2.0x"）
            if (element.fill && r >= 34) {
                wchar_t zoomBuf[24]{};
                if (std::abs(zoom - std::round(zoom)) < 0.05f) {
                    swprintf_s(zoomBuf, L"%.0fx", zoom);
                } else {
                    swprintf_s(zoomBuf, L"%.1fx", zoom);
                }
                const float pillW = 36.0f * dpi;
                const float pillH = 18.0f * dpi;
                const float pillX = cx - pillW * 0.5f;
                const float pillY = cy + rf - borderW - pillH - 4.5f * dpi;

                Gdiplus::GraphicsPath pillPath;
                buildRoundedPath(pillPath, pillX, pillY, pillW, pillH, pillH * 0.5f);
                Gdiplus::SolidBrush pillBg(Gdiplus::Color(186, 15, 23, 42));
                Gdiplus::Pen pillBorder(Gdiplus::Color(92, 255, 255, 255), 0.95f);
                g.FillPath(&pillBg, &pillPath);
                g.DrawPath(&pillBorder, &pillPath);

                Gdiplus::FontFamily fam(L"Segoe UI");
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
                Gdiplus::PointF(cx, cy - rf - 2.0f),
                Gdiplus::PointF(cx, cy + rf + 2.0f),
                Gdiplus::Color(252, topR, topG, topB),
                Gdiplus::Color(252, botR, botG, botB));
            Gdiplus::Pen mainRimPen(&bezelGrad, borderW);
            Gdiplus::Pen innerRimPen(rimLum > 0.72f
                                         ? Gdiplus::Color(88, 15, 23, 42)
                                         : Gdiplus::Color(155, 255, 255, 255),
                                     0.95f * sqrtDpi);

            const float inR = std::max(4.0f, rf - borderW * 0.52f);
            const float outR = rf + borderW * 0.45f;
            Gdiplus::Pen outerHairlinePen(Gdiplus::Color(55, 255, 255, 255), 0.75f);

            if (isRoundedSquare) {
                Gdiplus::GraphicsPath pMain, pInner, pOut;
                buildRoundedPath(pMain, cx - rf, cy - rf, rf * 2.0f, rf * 2.0f, cornerR);
                buildRoundedPath(pInner, cx - inR, cy - inR, inR * 2.0f, inR * 2.0f,
                                 std::max(2.0f, cornerR - borderW * 0.5f));
                buildRoundedPath(pOut, cx - outR, cy - outR, outR * 2.0f, outR * 2.0f, cornerR + borderW * 0.45f);
                g.DrawPath(&outerContourPen, &pMain);
                g.DrawPath(&mainRimPen, &pMain);
                g.DrawPath(&innerRimPen, &pInner);
                if (rimLum <= 0.72f) g.DrawPath(&outerHairlinePen, &pOut);
            } else {
                g.DrawEllipse(&outerContourPen, cx - rf, cy - rf, rf * 2.0f, rf * 2.0f);
                g.DrawEllipse(&mainRimPen, cx - rf, cy - rf, rf * 2.0f, rf * 2.0f);
                g.DrawEllipse(&innerRimPen, cx - inR, cy - inR, inR * 2.0f, inR * 2.0f);
                if (rimLum <= 0.72f) g.DrawEllipse(&outerHairlinePen, cx - outR, cy - outR, outR * 2.0f, outR * 2.0f);
            }

            // (e) 透镜端三层同心微宝石卡扣锚点（精准扣合在单轴引线与金属镜框的交接点，不被底层悬浮阴影压暗）
            if (hasLeaderBridge) {
                const float ax = cx - leaderUx * lensEdgeDist;
                const float ay = cy - leaderUy * lensEdgeDist;
                drawJewelAnchor(g, ax, ay, 2.9f * sqrtDpi);
            }
        }

        blendOverlay(canvas, lensLayer, lensCenter.x - r - pad, lensCenter.y - r - pad);
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
        if (element.spotlightEllipse) {
            const cv::Point center((x1 + x2) / 2, (y1 + y2) / 2);
            const cv::Size axes((std::max)(1, (x2 - x1) / 2), (std::max)(1, (y2 - y1) / 2));
            cv::ellipse(canvas, center, axes, 0, 0, 360, col, thick, cv::LINE_AA);
        } else {
            const float r = (std::min)({element.cornerRadius, (x2 - x1) * 0.5f, (y2 - y1) * 0.5f});
            if (r <= 0.5f) {
                cv::rectangle(canvas, cv::Rect(x1, y1, x2 - x1, y2 - y1), col, thick, cv::LINE_AA);
            } else {
                const int ir = static_cast<int>(std::lround(r));
                cv::line(canvas, cv::Point(x1 + ir, y1), cv::Point(x2 - ir, y1), col, thick, cv::LINE_AA);
                cv::line(canvas, cv::Point(x1 + ir, y2), cv::Point(x2 - ir, y2), col, thick, cv::LINE_AA);
                cv::line(canvas, cv::Point(x1, y1 + ir), cv::Point(x1, y2 - ir), col, thick, cv::LINE_AA);
                cv::line(canvas, cv::Point(x2, y1 + ir), cv::Point(x2, y2 - ir), col, thick, cv::LINE_AA);
                cv::ellipse(canvas, cv::Point(x1 + ir, y1 + ir), cv::Size(ir, ir), 180, 0, 90, col, thick, cv::LINE_AA);
                cv::ellipse(canvas, cv::Point(x2 - ir, y1 + ir), cv::Size(ir, ir), 270, 0, 90, col, thick, cv::LINE_AA);
                cv::ellipse(canvas, cv::Point(x2 - ir, y2 - ir), cv::Size(ir, ir), 0, 0, 90, col, thick, cv::LINE_AA);
                cv::ellipse(canvas, cv::Point(x1 + ir, y2 - ir), cv::Size(ir, ir), 90, 0, 90, col, thick, cv::LINE_AA);
            }
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

                Gdiplus::FontFamily fontFamily(L"Microsoft YaHei UI");
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
        double fontScale = 0.6;
        cv::Size textSz = cv::getTextSize(element.watermarkText, cv::FONT_HERSHEY_SIMPLEX,
                                           fontScale, 1, &baseline);

        int diag = static_cast<int>(std::sqrt(roiW * roiW + roiH * roiH)) + textSz.width;
        cv::Mat wmCanvas(diag * 2, diag * 2, canvas.type(), cv::Scalar(0, 0, 0, 0));

        int spacing = std::max(element.watermarkSpacing, textSz.width + 20);
        int vSpacing = textSz.height + spacing / 2;
        cv::Scalar wmColor(200, 200, 200);

        for (int y = 0; y < wmCanvas.rows; y += vSpacing) {
            for (int x = 0; x < wmCanvas.cols; x += spacing) {
                cv::putText(wmCanvas, element.watermarkText, cv::Point(x, y + textSz.height),
                            cv::FONT_HERSHEY_SIMPLEX, fontScale, wmColor, 1, cv::LINE_AA);
            }
        }

        cv::Point2f wmCenter(static_cast<float>(wmCanvas.cols) / 2.0f, static_cast<float>(wmCanvas.rows) / 2.0f);
        cv::Mat rotMat = cv::getRotationMatrix2D(wmCenter, element.watermarkAngle, 1.0);
        cv::Mat wmRotated;
        cv::warpAffine(wmCanvas, wmRotated, rotMat, wmCanvas.size(), cv::INTER_LINEAR, cv::BORDER_CONSTANT);

        int cropX = wmRotated.cols / 2 - roiW / 2;
        int cropY = wmRotated.rows / 2 - roiH / 2;
        cropX = std::max(0, std::min(cropX, wmRotated.cols - roiW));
        cropY = std::max(0, std::min(cropY, wmRotated.rows - roiH));
        cv::Mat wmCrop = wmRotated(cv::Rect(cropX, cropY, roiW, roiH));

        cv::Rect dstRoi(x1, y1, roiW, roiH);
        cv::Mat canvasRoi = canvas(dstRoi);
        double wmAlpha = static_cast<double>(element.watermarkOpacity);
        cv::addWeighted(wmCrop, wmAlpha, canvasRoi, 1.0, 0, canvasRoi);
    }
};

class InpaintHandler : public DefaultBoxHandler {
public:
    void render(cv::Mat& canvas, const MarkupElement& element) const override {
        LOG_DEBUG("标注引擎: 渲染智能消除 radius={}", element.inpaintRadius);

        int x1 = std::min(element.startPt.x, element.endPt.x);
        int y1 = std::min(element.startPt.y, element.endPt.y);
        int x2 = std::max(element.startPt.x, element.endPt.x);
        int y2 = std::max(element.startPt.y, element.endPt.y);
        x1 = std::max(0, x1); y1 = std::max(0, y1);
        x2 = std::min(canvas.cols, x2); y2 = std::min(canvas.rows, y2);
        if (x2 <= x1 || y2 <= y1) return;

        // 仅在目标选区外扩邻域子块 (Local Padded ROI) 上执行 TELEA 图像修复，避免全屏大图转换与全图扫描开销
        const int pad = std::max(16, static_cast<int>(std::ceil(element.inpaintRadius * 3.0f)));
        const int rx1 = std::max(0, x1 - pad);
        const int ry1 = std::max(0, y1 - pad);
        const int rx2 = std::min(canvas.cols, x2 + pad);
        const int ry2 = std::min(canvas.rows, y2 + pad);
        const cv::Rect subRect(rx1, ry1, rx2 - rx1, ry2 - ry1);
        const cv::Rect innerRect(x1 - rx1, y1 - ry1, x2 - x1, y2 - y1);

        cv::Mat subCanvas = canvas(subRect);
        cv::Mat mask = cv::Mat::zeros(subCanvas.size(), CV_8UC1);
        cv::rectangle(mask, innerRect, cv::Scalar(255), cv::FILLED);

        cv::Mat bgr;
        if (subCanvas.channels() == 4) {
            cv::cvtColor(subCanvas, bgr, cv::COLOR_BGRA2BGR);
        } else {
            bgr = subCanvas;
        }

        cv::Mat result;
        cv::inpaint(bgr, mask, result, element.inpaintRadius, cv::INPAINT_TELEA);

        cv::Rect dstRoi(x1, y1, x2 - x1, y2 - y1);
        if (canvas.channels() == 4) {
            cv::Mat result4;
            cv::cvtColor(result(innerRect), result4, cv::COLOR_BGR2BGRA);
            result4.copyTo(canvas(dstRoi));
        } else {
            result(innerRect).copyTo(canvas(dstRoi));
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

void MarkupEngine::renderAll(cv::Mat& canvas, bool includeActiveHandles) const {
    if (canvas.empty() || m_elements.empty()) return;

    auto isBaseFilterTool = [](MarkupTool t) noexcept {
        return t == MarkupTool::Mosaic || t == MarkupTool::Blur || t == MarkupTool::Inpaint;
    };

    std::vector<const MarkupElement*> spotlights;
    for (const auto& elem : m_elements) {
        if (!elem) continue;
        if (elem->tool == MarkupTool::Spotlight) {
            spotlights.push_back(elem.get());
        } else if (isBaseFilterTool(elem->tool)) {
            if (const auto* handler = MarkupToolRegistry::instance().getHandler(elem->tool)) {
                handler->render(canvas, *elem);
            }
        }
    }

    // 聚光灯联合遮罩层：多区域求亮区并集一次性弱化背景，且保证后续前景箭头/文字/序号始终明亮清晰
    if (!spotlights.empty()) {
        SpotlightHandler::renderUnified(canvas, spotlights);
    }

    for (const auto& elem : m_elements) {
        if (!elem || elem->tool == MarkupTool::Spotlight || isBaseFilterTool(elem->tool)) continue;
        if (const auto* handler = MarkupToolRegistry::instance().getHandler(elem->tool)) {
            handler->render(canvas, *elem);
        }
    }

    if (includeActiveHandles) {
        for (const auto& elem : m_elements) {
            if (elem && elem->isActive) {
                if (const auto* handler = MarkupToolRegistry::instance().getHandler(elem->tool)) {
                    handler->renderActiveHandles(canvas, *elem);
                }
            }
        }
    }
}

void MarkupEngine::renderElement(cv::Mat& canvas, const MarkupElement& element, bool includeActiveHandles) const {
    if (const auto* handler = MarkupToolRegistry::instance().getHandler(element.tool)) {
        handler->render(canvas, element);
        if (includeActiveHandles && element.isActive) {
            handler->renderActiveHandles(canvas, element);
        }
    }
}

}  // namespace tools3000::capture
