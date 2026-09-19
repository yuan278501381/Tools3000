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
#include "capture/CornerRadiusHelper.h"
#include "core/logger/Logger.h"
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
        if (g_gdiPlusToken == 0) {
            int w = text.empty() ? 140 : static_cast<int>((std::max)(40.0f, fontSize * 2.0f));
            int h = static_cast<int>((std::max)(24.0f, fontSize * 1.3f));
            return cv::Size((std::max)(w, 24), (std::max)(h, fontSize + 8));
        }

        Gdiplus::FontFamily fontFamily(L"Microsoft YaHei UI");
        const Gdiplus::FontFamily* activeFamily = &fontFamily;
        Gdiplus::FontFamily fallbackFamily(L"Segoe UI");
        if (!fontFamily.IsAvailable()) {
            activeFamily = &fallbackFamily;
        }
        Gdiplus::Font font(activeFamily, static_cast<Gdiplus::REAL>(fontSize), Gdiplus::FontStyleBold, Gdiplus::UnitPixel);
        Gdiplus::StringFormat format(Gdiplus::StringFormat::GenericTypographic());
        format.SetAlignment(Gdiplus::StringAlignmentNear);
        format.SetLineAlignment(Gdiplus::StringAlignmentNear);

        Gdiplus::Bitmap tmpBmp(1, 1, PixelFormat32bppARGB);
        Gdiplus::Graphics g(&tmpBmp);
        Gdiplus::RectF boundRect;
        std::wstring measureStr = text.empty() ? L"点击输入文字..." : text;
        g.MeasureString(measureStr.c_str(), -1, &font, Gdiplus::PointF(0, 0), &format, &boundRect);
        int w = static_cast<int>(std::ceil(boundRect.Width)) + 16;
        int h = static_cast<int>(std::ceil(boundRect.Height)) + 8;
        return cv::Size((std::max)(w, 24), (std::max)(h, fontSize + 8));
    }

    void renderSnipasteStyleText(cv::Mat& canvas, const std::string& text, cv::Point pt, const cv::Scalar& color,
                                 int fontSize, bool isEditing, bool withBackdrop, cv::Size& outSize,
                                 bool textOutline = true, const tools3000::capture::MarkupColor& outlineColor = tools3000::capture::MarkupColor::Auto()) {
        ensureGdiplusStartup();
        std::wstring wtext = utf8ToWide(text);
        if (wtext.empty() && !isEditing) {
            outSize = cv::Size(0, 0);
            return;
        }

        cv::Size sz = measureSnipasteText(wtext, fontSize, isEditing);
        outSize = sz;
        if (sz.width <= 0 || sz.height <= 0) return;

        cv::Mat textMat(sz, CV_8UC4, cv::Scalar(0, 0, 0, 0));
        {
            Gdiplus::Bitmap bitmap(sz.width, sz.height, static_cast<INT>(textMat.step), PixelFormat32bppARGB, textMat.data);
            Gdiplus::Graphics g(&bitmap);
            g.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
            // 采用纯净灰度抗锯齿 (TextRenderingHintAntiAliasGridFit)，严禁在透明图层上使用 ClearType 造成彩色/黑白栅格条纹
            g.SetTextRenderingHint(Gdiplus::TextRenderingHintAntiAliasGridFit);

            BYTE rVal = static_cast<BYTE>(color[2]);
            BYTE gVal = static_cast<BYTE>(color[1]);
            BYTE bVal = static_cast<BYTE>(color[0]);
            float luminance = (0.299f * rVal + 0.587f * gVal + 0.114f * bVal) / 255.0f;

            // 1. 文字绘制 (纯透明底衬 + 高对比描边 + 矢量文字填充)
            Gdiplus::FontFamily fontFamily(L"Microsoft YaHei UI");
            const Gdiplus::FontFamily* activeFamily = &fontFamily;
            Gdiplus::FontFamily fallbackFamily(L"Segoe UI");
            if (!fontFamily.IsAvailable()) {
                activeFamily = &fallbackFamily;
            }

            Gdiplus::StringFormat format(Gdiplus::StringFormat::GenericTypographic());
            format.SetAlignment(Gdiplus::StringAlignmentNear);
            format.SetLineAlignment(Gdiplus::StringAlignmentNear);

            float textX = 6.0f;
            float textY = 4.0f;

            Gdiplus::GraphicsPath boxPath;
            float padX = 5.0f, padY = 3.0f;
            float r = 6.0f;
            float bx = textX - padX, by = textY - padY, bw = sz.width - 4.0f, bh = sz.height - 2.0f;
            boxPath.AddArc(bx, by, r * 2, r * 2, 180, 90);
            boxPath.AddArc(bx + bw - r * 2, by, r * 2, r * 2, 270, 90);
            boxPath.AddArc(bx + bw - r * 2, by + bh - r * 2, r * 2, r * 2, 0, 90);
            boxPath.AddArc(bx, by + bh - r * 2, r * 2, r * 2, 90, 90);
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
                Gdiplus::Pen editFocusPen(Gdiplus::Color(200, 59, 130, 246), 1.5f);
                editFocusPen.SetDashStyle(Gdiplus::DashStyleDash);
                g.DrawPath(&editFocusPen, &boxPath);
            }

            if (!wtext.empty()) {
                Gdiplus::GraphicsPath textPath;
                textPath.AddString(wtext.c_str(), -1, activeFamily, Gdiplus::FontStyleBold, static_cast<Gdiplus::REAL>(fontSize), Gdiplus::PointF(textX, textY), &format);

                // 文字高精度外扩描边轮廓渲染 (先绘制外扩描边轮廓，再绘制内芯文字，确保高反差清晰度)
                if (textOutline) {
                    Gdiplus::Color strokeColor;
                    if (outlineColor.isAuto()) {
                        strokeColor = (luminance > 0.45f) ? Gdiplus::Color(250, 15, 17, 23) : Gdiplus::Color(250, 255, 255, 255);
                    } else {
                        BYTE a = outlineColor.a > 0 ? outlineColor.a : 250;
                        strokeColor = Gdiplus::Color(a, outlineColor.r, outlineColor.g, outlineColor.b);
                    }
                    float strokeWidth = std::max(3.0f, fontSize * 0.16f);
                    Gdiplus::Pen strokePen(strokeColor, strokeWidth);
                    strokePen.SetLineJoin(Gdiplus::LineJoinRound);
                    g.DrawPath(&strokePen, &textPath);
                }

                // 正文矢量文字填充
                Gdiplus::SolidBrush textBrush(Gdiplus::Color(255, rVal, gVal, bVal));
                g.FillPath(&textBrush, &textPath);
            } else if (isEditing) {
                // 空文本编辑态：绘制提示占位文字“点击输入文字...”
                Gdiplus::Font placeholderFont(activeFamily, static_cast<Gdiplus::REAL>(fontSize * 0.88f), Gdiplus::FontStyleRegular, Gdiplus::UnitPixel);
                Gdiplus::SolidBrush placeholderBrush(Gdiplus::Color(180, 148, 163, 184));
                g.DrawString(L"点击输入文字...", -1, &placeholderFont, Gdiplus::PointF(textX + 2.0f, textY + 1.0f), &format, &placeholderBrush);
            }

            // 2. 编辑态光标绘制
            if (isEditing) {
                bool showCursor = ((GetTickCount() / 450) % 2 == 0) || wtext.empty();
                if (showCursor) {
                    float cursorX = textX + 2.0f;
                    if (!wtext.empty()) {
                        Gdiplus::Font font(activeFamily, static_cast<Gdiplus::REAL>(fontSize), Gdiplus::FontStyleBold, Gdiplus::UnitPixel);
                        Gdiplus::RectF measured;
                        g.MeasureString(wtext.c_str(), -1, &font, Gdiplus::PointF(0, 0), &format, &measured);
                        cursorX = textX + measured.Width + 2.0f;
                    }
                    Gdiplus::Pen cursorPen(Gdiplus::Color(255, 59, 130, 246), 2.0f);
                    g.DrawLine(&cursorPen, cursorX, textY + 2.0f, cursorX, textY + static_cast<float>(fontSize) + 2.0f);
                }
            }
        }

        // 4. 高性能自适应 Alpha 混合到 canvas (精准处理 3 通道 BGR 与 4 通道 BGRA，杜绝跨步错位栅格)
        blendOverlay(canvas, textMat, pt.x, pt.y);
    }

    void renderSnipasteStyleNumberBadge(cv::Mat& canvas, int number, cv::Point center, const cv::Scalar& color, bool fill = true, float dpiScale = 1.0f, tools3000::capture::NumberBadgeShape shape = tools3000::capture::NumberBadgeShape::Circle) {
        ensureGdiplusStartup();
        float scale = dpiScale > 0.0f ? dpiScale : 1.0f;
        int radius = static_cast<int>(std::round(15.0f * scale));
        int sz = radius * 2 + static_cast<int>(std::round(10.0f * scale));
        cv::Mat badgeMat(sz, sz, CV_8UC4, cv::Scalar(0, 0, 0, 0));
        {
            Gdiplus::Bitmap bitmap(sz, sz, static_cast<INT>(badgeMat.step), PixelFormat32bppARGB, badgeMat.data);
            Gdiplus::Graphics g(&bitmap);
            g.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
            g.SetTextRenderingHint(Gdiplus::TextRenderingHintAntiAlias);

            float cx = sz / 2.0f;
            float cy = sz / 2.0f;

            BYTE rVal = static_cast<BYTE>(color[2]);
            BYTE gVal = static_cast<BYTE>(color[1]);
            BYTE bVal = static_cast<BYTE>(color[0]);

            if (shape == tools3000::capture::NumberBadgeShape::RoundedSquare) {
                // 方角微晶胶囊模式 (Modern Rounded Square / Glass Capsule)
                float boxSz = radius * 2.0f;
                float r = 5.0f * scale;
                float bx = cx - radius;
                float by = cy - radius;

                Gdiplus::GraphicsPath squarePath;
                squarePath.AddArc(bx, by, r * 2, r * 2, 180, 90);
                squarePath.AddArc(bx + boxSz - r * 2, by, r * 2, r * 2, 270, 90);
                squarePath.AddArc(bx + boxSz - r * 2, by + boxSz - r * 2, r * 2, r * 2, 0, 90);
                squarePath.AddArc(bx, by + boxSz - r * 2, r * 2, r * 2, 90, 90);
                squarePath.CloseFigure();

                if (fill) {
                    Gdiplus::SolidBrush shadowBrush(Gdiplus::Color(100, 0, 0, 0));
                    g.TranslateTransform(1.5f * scale, 2.5f * scale);
                    g.FillPath(&shadowBrush, &squarePath);
                    g.ResetTransform();

                    Gdiplus::SolidBrush fillBrush(Gdiplus::Color(255, rVal, gVal, bVal));
                    g.FillPath(&fillBrush, &squarePath);

                    Gdiplus::Pen ringPen(Gdiplus::Color(220, 255, 255, 255), 1.2f * scale);
                    g.DrawPath(&ringPen, &squarePath);
                } else {
                    Gdiplus::SolidBrush bgBrush(Gdiplus::Color(230, 255, 255, 255));
                    g.FillPath(&bgBrush, &squarePath);

                    Gdiplus::Pen outlinePen(Gdiplus::Color(255, rVal, gVal, bVal), 2.2f * scale);
                    g.DrawPath(&outlinePen, &squarePath);
                }
            } else {
                if (fill) {
                    // 1. 实心圆形：柔和外层投影 + 主题色彩圆盘 + 1.5px 白色高光内圈
                    Gdiplus::SolidBrush shadowBrush(Gdiplus::Color(100, 0, 0, 0));
                    g.FillEllipse(&shadowBrush, cx - radius + 1.5f * scale, cy - radius + 2.5f * scale, radius * 2.0f, radius * 2.0f);

                    Gdiplus::SolidBrush circleBrush(Gdiplus::Color(255, rVal, gVal, bVal));
                    g.FillEllipse(&circleBrush, cx - radius, cy - radius, radius * 2.0f, radius * 2.0f);

                    Gdiplus::Pen ringPen(Gdiplus::Color(220, 255, 255, 255), 1.5f * scale);
                    g.DrawEllipse(&ringPen, cx - radius + 1.0f * scale, cy - radius + 1.0f * scale, (radius - 1.0f * scale) * 2.0f, (radius - 1.0f * scale) * 2.0f);
                } else {
                    // 2. 空心圆形：半透明纯白底 + 主题色彩外环
                    Gdiplus::SolidBrush bgBrush(Gdiplus::Color(230, 255, 255, 255));
                    g.FillEllipse(&bgBrush, cx - radius, cy - radius, radius * 2.0f, radius * 2.0f);

                    Gdiplus::Pen outlinePen(Gdiplus::Color(255, rVal, gVal, bVal), 2.2f * scale);
                    g.DrawEllipse(&outlinePen, cx - radius + 1.0f * scale, cy - radius + 1.0f * scale, (radius - 1.0f * scale) * 2.0f, (radius - 1.0f * scale) * 2.0f);
                }
            }

            // 4. 数字矢量排版
            std::wstring numWStr = std::to_wstring(number);
            Gdiplus::FontFamily fontFamily(L"Segoe UI");
            float fontSize = ((number >= 100) ? 11.0f : (number >= 10 ? 13.0f : 15.0f)) * scale;
            Gdiplus::Font font(&fontFamily, fontSize, Gdiplus::FontStyleBold, Gdiplus::UnitPixel);

            Gdiplus::StringFormat format;
            format.SetAlignment(Gdiplus::StringAlignmentCenter);
            format.SetLineAlignment(Gdiplus::StringAlignmentCenter);

            Gdiplus::SolidBrush numBrush(fill ? Gdiplus::Color(255, 255, 255, 255) : Gdiplus::Color(255, rVal, gVal, bVal));
            Gdiplus::RectF layoutRect(cx - radius, cy - radius - 0.5f * scale, radius * 2.0f, radius * 2.0f);
            g.DrawString(numWStr.c_str(), -1, &font, layoutRect, &format, &numBrush);
        }

        int startX = center.x - sz / 2;
        int startY = center.y - sz / 2;
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

void MarkupEngine::setBaseImage(const cv::Mat& image) {
    m_baseImage = image.clone();
    m_elements.clear();
    m_undoStack.clear();
    m_nextNumber = 1;
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

    // 重新校准序号计数器：取当前所有存活 Number 元素的最大编号 + 1
    int maxNum = 0;
    for (const auto& elem : m_elements) {
        if (elem->tool == MarkupTool::Number) {
            maxNum = std::max(maxNum, elem->numberValue);
        }
    }
    m_nextNumber = maxNum + 1;

    LOG_DEBUG("标注引擎: 撤销, 剩余元素数={}", m_elements.size());
    return true;
}

bool MarkupEngine::redo() {
    if (m_undoStack.empty()) return false;

    auto restored = std::move(m_undoStack.back());
    if (restored->tool == MarkupTool::Number) {
        m_nextNumber = std::max(m_nextNumber, restored->numberValue + 1);
    }
    m_elements.push_back(std::move(restored));
    m_undoStack.pop_back();
    LOG_DEBUG("标注引擎: 重做, 剩余元素数={}", m_elements.size());
    return true;
}

void MarkupEngine::clearAll() {
    m_elements.clear();
    m_undoStack.clear();
    m_nextNumber = 1;
}

void MarkupEngine::removeElement(uint32_t id) {
    m_elements.erase(std::remove_if(m_elements.begin(), m_elements.end(),
        [id](const std::unique_ptr<MarkupElement>& e) { return e->id == id; }),
        m_elements.end());

    int maxNum = 0;
    for (const auto& elem : m_elements) {
        if (elem->tool == MarkupTool::Number) {
            maxNum = std::max(maxNum, elem->numberValue);
        }
    }
    m_nextNumber = maxNum + 1;
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
    return addElement(std::move(elem));
}

MarkupElement* MarkupEngine::applyBlur(cv::Point p1, cv::Point p2, int kernelSize) {
    auto elem = std::make_unique<MarkupElement>();
    elem->tool = MarkupTool::Blur;
    elem->startPt = cv::Point((std::min)(p1.x, p2.x), (std::min)(p1.y, p2.y));
    elem->endPt   = cv::Point((std::max)(p1.x, p2.x), (std::max)(p1.y, p2.y));
    elem->mosaicBlockSize = kernelSize;
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

MarkupElement* MarkupEngine::drawNumberMark(cv::Point position, MarkupColor color, float dpiScale, NumberBadgeShape shape) {
    auto elem = std::make_unique<MarkupElement>();
    elem->tool = MarkupTool::Number;
    elem->startPt = position;
    elem->endPt = position;
    elem->color = color;
    elem->numberValue = m_nextNumber++;
    elem->dpiScale = dpiScale;
    elem->numberShape = shape;
    return addElement(std::move(elem));
}

int MarkupEngine::addNumberMark(cv::Point position, MarkupColor color, float dpiScale, NumberBadgeShape shape) {
    auto* elem = drawNumberMark(position, color, dpiScale, shape);
    return elem ? elem->numberValue : 0;
}

void MarkupEngine::addMagnifier(cv::Point center, float scale, int radius) {
    auto elem = std::make_unique<MarkupElement>();
    elem->tool = MarkupTool::Magnifier;
    elem->startPt = center;
    elem->magnifierScale = scale;
    elem->magnifierRadius = radius;
    addElement(std::move(elem));
}

void MarkupEngine::addSpotlight(cv::Point p1, cv::Point p2, MarkupColor color, float dimAlpha, bool ellipse) {
    auto elem = std::make_unique<MarkupElement>();
    elem->tool = MarkupTool::Spotlight;
    elem->startPt = cv::Point((std::min)(p1.x, p2.x), (std::min)(p1.y, p2.y));
    elem->endPt   = cv::Point((std::max)(p1.x, p2.x), (std::max)(p1.y, p2.y));
    elem->color = color;
    elem->spotlightDimAlpha = dimAlpha;
    elem->spotlightEllipse = ellipse;
    addElement(std::move(elem));
    LOG_DEBUG("标注引擎: 添加聚光灯 ({},{})→({},{}) dimAlpha={:.2f} ellipse={}",
              p1.x, p1.y, p2.x, p2.y, dimAlpha, ellipse);
}

void MarkupEngine::addWatermark(cv::Point p1, cv::Point p2, const std::string& text, float opacity, float angle) {
    auto elem = std::make_unique<MarkupElement>();
    elem->tool = MarkupTool::Watermark;
    elem->startPt = cv::Point((std::min)(p1.x, p2.x), (std::min)(p1.y, p2.y));
    elem->endPt   = cv::Point((std::max)(p1.x, p2.x), (std::max)(p1.y, p2.y));
    elem->watermarkText = text;
    elem->watermarkOpacity = opacity;
    elem->watermarkAngle = angle;
    addElement(std::move(elem));
    LOG_DEBUG("标注引擎: 添加水印 ({},{})→({},{}) text='{}' opacity={:.2f} angle={:.1f}",
              p1.x, p1.y, p2.x, p2.y, text, opacity, angle);
}

void MarkupEngine::applyInpaint(cv::Point p1, cv::Point p2, int radius) {
    auto elem = std::make_unique<MarkupElement>();
    elem->tool = MarkupTool::Inpaint;
    elem->startPt = cv::Point((std::min)(p1.x, p2.x), (std::min)(p1.y, p2.y));
    elem->endPt   = cv::Point((std::max)(p1.x, p2.x), (std::max)(p1.y, p2.y));
    elem->inpaintRadius = radius;
    addElement(std::move(elem));
    LOG_DEBUG("标注引擎: 添加智能消除 ({},{})→({},{}) radius={}",
              p1.x, p1.y, p2.x, p2.y, radius);
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
        DefaultBoxHandler::renderActiveHandles(canvas, element);

        int x1 = (std::min)(element.startPt.x, element.endPt.x);
        int y1 = (std::min)(element.startPt.y, element.endPt.y);
        int x2 = (std::max)(element.startPt.x, element.endPt.x);
        int y2 = (std::max)(element.startPt.y, element.endPt.y);
        float w = static_cast<float>(x2 - x1);
        float h = static_cast<float>(y2 - y1);

        if (CornerRadiusHelper::canShowHandles(w, h, 1.0f, 40.0f)) {
            bool isSmall = (w < 80.0f || h < 80.0f);
            auto handles = CornerRadiusHelper::getCornerHandles(
                static_cast<float>(x1), static_cast<float>(y1),
                static_cast<float>(x2), static_cast<float>(y2),
                element.cornerRadius, 1.0f, isSmall);

            cv::Scalar activeColor(255, 140, 0); // Azure / Deep sky blue
            for (const auto& hnd : handles) {
                cv::Point cpt(static_cast<int>(std::round(hnd.x)), static_cast<int>(std::round(hnd.y)));
                // 外圈纯白高反差实心圆
                cv::circle(canvas, cpt, 5, cv::Scalar(255, 255, 255), cv::FILLED, cv::LINE_AA);
                // 细描边
                cv::circle(canvas, cpt, 5, activeColor, 1, cv::LINE_AA);
                // 中心黑曜石微圆点
                cv::circle(canvas, cpt, 2, cv::Scalar(40, 25, 15), cv::FILLED, cv::LINE_AA);
            }
        }
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
            const_cast<MarkupElement&>(element).textRenderSize = measureSnipasteText(wtext, static_cast<int>(element.fontSize), element.isEditing);
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

    void resize(MarkupElement& element, int dx, int dy, HitArea handle) const override {
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

    void render(cv::Mat& canvas, const MarkupElement& element) const override {
        auto color = element.color.toCvScalar();
        cv::Size renderedSize(0, 0);
        bool showPlaceholder = element.isEditing || element.isActive;
        renderSnipasteStyleText(canvas, element.text, element.startPt, color, static_cast<int>(element.fontSize),
                                showPlaceholder, element.fill, renderedSize,
                                element.textOutline, element.textOutlineColor);
        const_cast<MarkupElement&>(element).textRenderSize = renderedSize;
        if (renderedSize.width > 0 && renderedSize.height > 0) {
            const_cast<MarkupElement&>(element).endPt = cv::Point(element.startPt.x + renderedSize.width, element.startPt.y + renderedSize.height);
        }
    }
};

class NumberHandler : public DefaultBoxHandler {
public:
    cv::Rect getBoundingBox(const MarkupElement& element) const override {
        int r = static_cast<int>(std::round(16.0f * (element.dpiScale > 0.0f ? element.dpiScale : 1.0f)));
        cv::Rect badgeBox(element.startPt.x - r, element.startPt.y - r, r * 2, r * 2);
        if (element.hasLeaderArrow) {
            int x1 = (std::min)(badgeBox.x, element.endPt.x - 8);
            int y1 = (std::min)(badgeBox.y, element.endPt.y - 8);
            int x2 = (std::max)(badgeBox.x + badgeBox.width, element.endPt.x + 8);
            int y2 = (std::max)(badgeBox.y + badgeBox.height, element.endPt.y + 8);
            return cv::Rect(x1, y1, x2 - x1, y2 - y1);
        }
        return badgeBox;
    }

    HitArea hitTest(const MarkupElement& element, cv::Point pt, int padding) const override {
        int r = static_cast<int>(std::round(16.0f * (element.dpiScale > 0.0f ? element.dpiScale : 1.0f)));
        double distBadge = std::hypot(pt.x - element.startPt.x, pt.y - element.startPt.y);
        if (distBadge <= r + padding) {
            return HitArea::Body;
        }
        if (element.hasLeaderArrow) {
            if (element.isActive) {
                int hw = 8;
                cv::Rect h2(element.endPt.x - hw, element.endPt.y - hw, hw * 2, hw * 2);
                if (h2.contains(pt)) return HitArea::RB; // Tip handle
            }
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
        } else {
            element.moveBy(dx, dy);
        }
    }

    void renderActiveHandles(cv::Mat& canvas, const MarkupElement& element) const override {
        if (!element.hasLeaderArrow) return;
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
        float scale = element.dpiScale > 0.0f ? element.dpiScale : 1.0f;
        int badgeR = static_cast<int>(std::round(15.0f * scale));

        if (element.hasLeaderArrow) {
            int thick = (std::max)(1, static_cast<int>(element.thickness));
            float dx = static_cast<float>(element.endPt.x - element.startPt.x);
            float dy = static_cast<float>(element.endPt.y - element.startPt.y);
            float dist = std::hypot(dx, dy);
            if (dist > static_cast<float>(badgeR)) {
                double angle = std::atan2(dy, dx);
                double headLen = (std::max)(11.0, thick * 3.5);
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

                drawPatternedLine(canvas, lineStart, basePt, color, thick, element.lineStyle);
            }
        }

        renderSnipasteStyleNumberBadge(canvas, element.numberValue, element.startPt, color, element.fill, element.dpiScale, element.numberShape);
    }
};

class MagnifierHandler : public DefaultBoxHandler {
public:
    cv::Rect getBoundingBox(const MarkupElement& element) const override {
        return cv::Rect(element.startPt.x - element.magnifierRadius, element.startPt.y - element.magnifierRadius, element.magnifierRadius * 2, element.magnifierRadius * 2);
    }

    void resize(MarkupElement& element, int dx, int dy, HitArea handle) const override {
        int delta = HandleGeometry::computeScalarDelta(dx, dy, handle);
        element.magnifierRadius += delta;
        if (element.magnifierRadius < 20) element.magnifierRadius = 20;
        if (element.magnifierRadius > 500) element.magnifierRadius = 500;
    }

    void render(cv::Mat& canvas, const MarkupElement& element) const override {
        int r = element.magnifierRadius;
        float scale = element.magnifierScale;
        int srcR = static_cast<int>(std::round(r / scale));
        if (r <= 0 || srcR <= 0) return;

        cv::Rect srcRect(element.startPt.x - srcR, element.startPt.y - srcR, srcR * 2, srcR * 2);
        cv::Rect dstRect(element.startPt.x - r, element.startPt.y - r, r * 2, r * 2);
        cv::Rect screenBounds(0, 0, canvas.cols, canvas.rows);

        cv::Rect validSrc = srcRect & screenBounds;
        cv::Rect validDst = dstRect & screenBounds;
        if (validSrc.area() <= 0 || validDst.area() <= 0) return;

        // 构建严格等比例正方形源补丁，消除边缘裁切引起的宽高比失真与拉伸形变
        cv::Mat srcPatch = cv::Mat::zeros(srcR * 2, srcR * 2, canvas.type());
        cv::Rect patchDst(validSrc.x - srcRect.x, validSrc.y - srcRect.y, validSrc.width, validSrc.height);
        canvas(validSrc).copyTo(srcPatch(patchDst));

        cv::Mat enlarged;
        cv::resize(srcPatch, enlarged, cv::Size(r * 2, r * 2), 0, 0, cv::INTER_CUBIC);

        cv::Mat mask = cv::Mat::zeros(r * 2, r * 2, CV_8UC1);
        cv::circle(mask, cv::Point(r, r), r - 2, cv::Scalar(255), cv::FILLED, cv::LINE_AA);

        cv::Rect patchSrc(validDst.x - dstRect.x, validDst.y - dstRect.y, validDst.width, validDst.height);
        cv::Mat dstROI = canvas(validDst);
        enlarged(patchSrc).copyTo(dstROI, mask(patchSrc));

        cv::circle(canvas, element.startPt, r, cv::Scalar(0, 0, 0), 2, cv::LINE_AA);
        cv::circle(canvas, element.startPt, r - 2, cv::Scalar(255, 255, 255), 2, cv::LINE_AA);

        int ch = 4;
        cv::line(canvas, cv::Point(element.startPt.x - ch, element.startPt.y), 
                         cv::Point(element.startPt.x + ch, element.startPt.y), 
                         cv::Scalar(0, 0, 0), 1, cv::LINE_AA);
        cv::line(canvas, cv::Point(element.startPt.x, element.startPt.y - ch), 
                         cv::Point(element.startPt.x, element.startPt.y + ch), 
                         cv::Scalar(0, 0, 0), 1, cv::LINE_AA);
    }
};

class SpotlightHandler : public DefaultBoxHandler {
public:
    void render(cv::Mat& canvas, const MarkupElement& element) const override {
        LOG_DEBUG("标注引擎: 渲染聚光灯 dimAlpha={:.2f} ellipse={}",
                  element.spotlightDimAlpha, element.spotlightEllipse);

        cv::Mat darkLayer(canvas.size(), canvas.type(), cv::Scalar(0, 0, 0, 255));
        int x1 = std::min(element.startPt.x, element.endPt.x);
        int y1 = std::min(element.startPt.y, element.endPt.y);
        int x2 = std::max(element.startPt.x, element.endPt.x);
        int y2 = std::max(element.startPt.y, element.endPt.y);
        x1 = std::max(0, x1); y1 = std::max(0, y1);
        x2 = std::min(canvas.cols, x2); y2 = std::min(canvas.rows, y2);

        if (x2 > x1 && y2 > y1) {
            if (element.spotlightEllipse) {
                cv::Point center((x1 + x2) / 2, (y1 + y2) / 2);
                cv::Size axes((x2 - x1) / 2, (y2 - y1) / 2);
                cv::Mat mask = cv::Mat::zeros(canvas.size(), CV_8UC1);
                cv::ellipse(mask, center, axes, 0, 0, 360, cv::Scalar(255), cv::FILLED, cv::LINE_AA);
                canvas.copyTo(darkLayer, mask);
            } else {
                cv::Rect roi(x1, y1, x2 - x1, y2 - y1);
                canvas(roi).copyTo(darkLayer(roi));
            }
        }

        double alpha = static_cast<double>(element.spotlightDimAlpha);
        cv::addWeighted(darkLayer, alpha, canvas, 1.0 - alpha, 0, canvas);
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

        int roiW = x2 - x1, roiH = y2 - y1;
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

        cv::Mat mask = cv::Mat::zeros(canvas.size(), CV_8UC1);
        cv::rectangle(mask, cv::Point(x1, y1), cv::Point(x2, y2), cv::Scalar(255), cv::FILLED);

        cv::Mat bgr;
        if (canvas.channels() == 4) {
            cv::cvtColor(canvas, bgr, cv::COLOR_BGRA2BGR);
        } else {
            bgr = canvas;
        }

        cv::Mat result;
        cv::inpaint(bgr, mask, result, element.inpaintRadius, cv::INPAINT_TELEA);

        if (canvas.channels() == 4) {
            cv::Mat result4;
            cv::cvtColor(result, result4, cv::COLOR_BGR2BGRA);
            cv::Rect roi(x1, y1, x2 - x1, y2 - y1);
            result4(roi).copyTo(canvas(roi));
        } else {
            cv::Rect roi(x1, y1, x2 - x1, y2 - y1);
            result(roi).copyTo(canvas(roi));
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
    for (const auto& elem : m_elements) {
        renderElement(canvas, *elem, includeActiveHandles);
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
