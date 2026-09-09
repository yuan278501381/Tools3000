#include "capture/CaptureVectorIcons.h"
#include <algorithm>
#include <cmath>
#include <vector>

namespace tools3000::capture {

void CaptureVectorIcons::renderIcon(ID2D1RenderTarget* rt, ID2D1Factory* factory,
                                    CaptureIconId iconId, const D2D1_RECT_F& rect,
                                    ID2D1Brush* brush, float scale) {
    if (!rt || !brush) return;

    const float cx = (rect.left + rect.right) * 0.5f;
    const float cy = (rect.top + rect.bottom) * 0.5f;
    const float btnW = rect.right - rect.left;
    const float btnH = rect.bottom - rect.top;
    if (btnW <= 1.0f || btnH <= 1.0f) return;

    // 黄金光学留白视口：收敛至 0.63 占比，在 32px 按钮中留出约 6~7px 通透负空间呼吸感
    const float s = (std::min)(btnW, btnH) / 24.0f * 0.63f;
    const float stroke = std::clamp(s * 2.12f, 1.42f * scale, 1.82f * scale);

    auto p = [&](float x, float y) -> D2D1_POINT_2F {
        return D2D1::Point2F(cx + (x - 12.0f) * s, cy + (y - 12.0f) * s);
    };

    auto makeSubBrush = [&](float alphaFactor) -> Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> {
        Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> srcSolid;
        Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> outBrush;
        if (SUCCEEDED(brush->QueryInterface(IID_PPV_ARGS(&srcSolid))) && srcSolid) {
            D2D1_COLOR_F c = srcSolid->GetColor();
            c.a *= alphaFactor;
            rt->CreateSolidColorBrush(c, outBrush.GetAddressOf());
        }
        return outBrush;
    };

    Microsoft::WRL::ComPtr<ID2D1StrokeStyle> roundStroke;
    if (factory) {
        D2D1_STROKE_STYLE_PROPERTIES props = D2D1::StrokeStyleProperties(
            D2D1_CAP_STYLE_ROUND,
            D2D1_CAP_STYLE_ROUND,
            D2D1_CAP_STYLE_ROUND,
            D2D1_LINE_JOIN_ROUND,
            10.0f
        );
        factory->CreateStrokeStyle(&props, nullptr, 0, roundStroke.GetAddressOf());
    }

    auto createPath = [&](const std::vector<D2D1_POINT_2F>& pts, bool closed) -> Microsoft::WRL::ComPtr<ID2D1PathGeometry> {
        if (!factory || pts.size() < 2) return nullptr;
        Microsoft::WRL::ComPtr<ID2D1PathGeometry> geo;
        if (FAILED(factory->CreatePathGeometry(geo.GetAddressOf()))) return nullptr;
        Microsoft::WRL::ComPtr<ID2D1GeometrySink> sink;
        if (FAILED(geo->Open(sink.GetAddressOf()))) return nullptr;
        sink->BeginFigure(pts[0], closed ? D2D1_FIGURE_BEGIN_FILLED : D2D1_FIGURE_BEGIN_HOLLOW);
        for (size_t i = 1; i < pts.size(); ++i) {
            sink->AddLine(pts[i]);
        }
        sink->EndFigure(closed ? D2D1_FIGURE_END_CLOSED : D2D1_FIGURE_END_OPEN);
        sink->Close();
        return geo;
    };

    auto drawPolyline = [&](const std::vector<D2D1_POINT_2F>& pts, bool closed, float w = -1.0f) {
        const float sw = (w > 0.0f) ? w : stroke;
        auto geo = createPath(pts, closed);
        if (geo) {
            rt->DrawGeometry(geo.Get(), brush, sw, roundStroke.Get());
        } else {
            for (size_t i = 1; i < pts.size(); ++i) {
                rt->DrawLine(pts[i - 1], pts[i], brush, sw, roundStroke.Get());
            }
            if (closed && pts.size() > 2) {
                rt->DrawLine(pts.back(), pts.front(), brush, sw, roundStroke.Get());
            }
        }
    };

    auto drawCircularArc = [&](D2D1_POINT_2F centerPt, float radius, float startDeg, float sweepDeg, float w = -1.0f) {
        const float sw = (w > 0.0f) ? w : stroke;
        const float deg2rad = 3.1415926535f / 180.0f;
        const float a0 = startDeg * deg2rad;
        const float a1 = (startDeg + sweepDeg) * deg2rad;
        const D2D1_POINT_2F startPt = D2D1::Point2F(centerPt.x + radius * std::cos(a0), centerPt.y + radius * std::sin(a0));
        const D2D1_POINT_2F endPt = D2D1::Point2F(centerPt.x + radius * std::cos(a1), centerPt.y + radius * std::sin(a1));

        if (factory) {
            Microsoft::WRL::ComPtr<ID2D1PathGeometry> geo;
            if (SUCCEEDED(factory->CreatePathGeometry(geo.GetAddressOf()))) {
                Microsoft::WRL::ComPtr<ID2D1GeometrySink> sink;
                if (SUCCEEDED(geo->Open(sink.GetAddressOf()))) {
                    sink->BeginFigure(startPt, D2D1_FIGURE_BEGIN_HOLLOW);
                    sink->AddArc(D2D1::ArcSegment(
                        endPt,
                        D2D1::SizeF(radius, radius),
                        0.0f,
                        sweepDeg >= 0.0f ? D2D1_SWEEP_DIRECTION_CLOCKWISE : D2D1_SWEEP_DIRECTION_COUNTER_CLOCKWISE,
                        std::fabs(sweepDeg) > 180.0f ? D2D1_ARC_SIZE_LARGE : D2D1_ARC_SIZE_SMALL
                    ));
                    sink->EndFigure(D2D1_FIGURE_END_OPEN);
                    sink->Close();
                    rt->DrawGeometry(geo.Get(), brush, sw, roundStroke.Get());
                    return;
                }
            }
        }
        const int steps = 16;
        D2D1_POINT_2F prev = startPt;
        for (int i = 1; i <= steps; ++i) {
            const float a = (startDeg + sweepDeg * (static_cast<float>(i) / steps)) * deg2rad;
            const D2D1_POINT_2F cur = D2D1::Point2F(centerPt.x + radius * std::cos(a), centerPt.y + radius * std::sin(a));
            rt->DrawLine(prev, cur, brush, sw, roundStroke.Get());
            prev = cur;
        }
    };

    switch (iconId) {
        // ── 1. 矩形工具 (光学视觉均衡圆角方形) ─────────────────────────────────────
        case CaptureIconId::ToolRectangle: {
            auto box = D2D1::RectF(cx - 7.8f * s, cy - 7.8f * s, cx + 7.8f * s, cy + 7.8f * s);
            rt->DrawRoundedRectangle(D2D1::RoundedRect(box, 2.6f * s, 2.6f * s), brush, stroke, roundStroke.Get());
            break;
        }

        // ── 1.1 直线工具 (带精致锚点的 45° 矢量线段) ───────────────────────────────
        case CaptureIconId::ToolLine: {
            rt->DrawLine(p(6.2f, 17.8f), p(17.8f, 6.2f), brush, stroke, roundStroke.Get());
            rt->FillEllipse(D2D1::Ellipse(p(5.2f, 18.8f), 1.8f * s, 1.8f * s), brush);
            rt->FillEllipse(D2D1::Ellipse(p(18.8f, 5.2f), 1.8f * s, 1.8f * s), brush);
            break;
        }

        // ── 2. 椭圆工具 (光学等重半径 8.4s) ────────────────────────────────────────
        case CaptureIconId::ToolEllipse: {
            auto ellipse = D2D1::Ellipse(D2D1::Point2F(cx, cy), 8.4f * s, 8.4f * s);
            rt->DrawEllipse(ellipse, brush, stroke, roundStroke.Get());
            break;
        }

        // ── 3. 45° 标注箭头 (流线型现代锐利指向箭头) ───────────────────────────────
        case CaptureIconId::ToolArrow: {
            rt->DrawLine(p(5.5f, 18.5f), p(17.8f, 6.2f), brush, stroke * 1.04f, roundStroke.Get());
            drawPolyline({ p(9.2f, 5.8f), p(18.2f, 5.8f), p(18.2f, 14.8f) }, false, stroke * 1.04f);
            break;
        }

        // ── 3.1 水平标准实心几何单向箭头 ───────────────────────────────────────────
        case CaptureIconId::ToolArrowStandard: {
            rt->DrawLine(p(4.0f, 12.0f), p(13.8f, 12.0f), brush, stroke, roundStroke.Get());
            auto head = createPath({ p(20.5f, 12.0f), p(13.2f, 7.2f), p(13.2f, 16.8f) }, true);
            if (head) {
                rt->FillGeometry(head.Get(), brush);
                rt->DrawGeometry(head.Get(), brush, 0.9f * scale, roundStroke.Get());
            } else {
                drawPolyline({ p(20.5f, 12.0f), p(13.2f, 7.2f), p(13.2f, 16.8f) }, true, stroke);
            }
            break;
        }

        // ── 4. 水平细线开放箭头 ────────────────────────────────────────────────────
        case CaptureIconId::ToolArrowThin: {
            rt->DrawLine(p(4.0f, 12.0f), p(19.2f, 12.0f), brush, stroke, roundStroke.Get());
            drawPolyline({ p(13.5f, 6.5f), p(19.5f, 12.0f), p(13.5f, 17.5f) }, false, stroke);
            break;
        }

        // ── 4.1 水平圆润科技机翼箭头 ────────────────────────────────────────────────
        case CaptureIconId::ToolArrowWing: {
            rt->DrawLine(p(4.0f, 12.0f), p(15.2f, 12.0f), brush, stroke, roundStroke.Get());
            auto wing = createPath({ p(20.5f, 12.0f), p(12.8f, 6.5f), p(15.5f, 12.0f), p(12.8f, 17.5f) }, true);
            if (wing) {
                rt->FillGeometry(wing.Get(), brush);
                rt->DrawGeometry(wing.Get(), brush, 0.9f * scale, roundStroke.Get());
            } else {
                drawPolyline({ p(20.5f, 12.0f), p(12.8f, 6.5f), p(15.5f, 12.0f), p(12.8f, 17.5f) }, true, stroke);
            }
            break;
        }

        // ── 5. 水平双向箭头 ────────────────────────────────────────────────────────
        case CaptureIconId::ToolArrowDouble: {
            rt->DrawLine(p(4.8f, 12.0f), p(19.2f, 12.0f), brush, stroke, roundStroke.Get());
            drawPolyline({ p(9.0f, 7.5f), p(4.5f, 12.0f), p(9.0f, 16.5f) }, false, stroke);
            drawPolyline({ p(15.0f, 7.5f), p(19.5f, 12.0f), p(15.0f, 16.5f) }, false, stroke);
            break;
        }

        // ── 6. 现代绘图画笔 (流线型笔身 + 平滑 S 型贝塞尔墨迹) ─────────────────────
        case CaptureIconId::ToolPen: {
            drawPolyline({
                p(16.2f, 3.2f),
                p(19.8f, 6.8f),
                p(8.6f, 18.0f),
                p(4.0f, 19.2f),
                p(5.2f, 14.6f)
            }, true, stroke);
            rt->DrawLine(p(13.6f, 5.8f), p(17.2f, 9.4f), brush, stroke * 0.88f, roundStroke.Get());
            if (factory) {
                Microsoft::WRL::ComPtr<ID2D1PathGeometry> wave;
                if (SUCCEEDED(factory->CreatePathGeometry(wave.GetAddressOf()))) {
                    Microsoft::WRL::ComPtr<ID2D1GeometrySink> sink;
                    if (SUCCEEDED(wave->Open(sink.GetAddressOf()))) {
                        sink->BeginFigure(p(11.6f, 19.6f), D2D1_FIGURE_BEGIN_HOLLOW);
                        sink->AddBezier(D2D1::BezierSegment(p(14.2f, 17.8f), p(17.2f, 21.2f), p(20.2f, 19.2f)));
                        sink->EndFigure(D2D1_FIGURE_END_OPEN);
                        sink->Close();
                        rt->DrawGeometry(wave.Get(), brush, stroke * 0.92f, roundStroke.Get());
                        break;
                    }
                }
            }
            rt->DrawLine(p(12.0f, 19.6f), p(20.0f, 19.6f), brush, stroke * 0.92f, roundStroke.Get());
            break;
        }

        // ── 7. 荧光笔 (对称楔形马克笔 + 柔和荧光色带) ───────────────────────────────
        case CaptureIconId::ToolHighlight: {
            drawPolyline({
                p(14.8f, 3.2f),
                p(19.8f, 8.2f),
                p(12.6f, 15.4f),
                p(7.6f, 10.4f)
            }, true, stroke);
            drawPolyline({
                p(7.6f, 10.4f),
                p(5.2f, 14.8f),
                p(8.2f, 16.8f),
                p(12.6f, 15.4f)
            }, false, stroke * 0.92f);
            if (auto bandBrush = makeSubBrush(0.35f)) {
                auto swath = D2D1::RectF(p(3.6f, 18.4f).x, p(3.6f, 18.4f).y, p(20.4f, 21.2f).x, p(20.4f, 21.2f).y);
                rt->FillRoundedRectangle(D2D1::RoundedRect(swath, 1.4f * s, 1.4f * s), bandBrush.Get());
            }
            rt->DrawLine(p(4.0f, 20.2f), p(20.0f, 20.2f), brush, stroke * 1.05f, roundStroke.Get());
            break;
        }

        // ── 8. 马赛克 (双透明度立体棋盘晶格：立刻呈现真实像素化质感) ───────────────
        case CaptureIconId::ToolMosaic: {
            auto box = D2D1::RectF(cx - 8.2f * s, cy - 8.2f * s, cx + 8.2f * s, cy + 8.2f * s);
            rt->DrawRoundedRectangle(D2D1::RoundedRect(box, 2.5f * s, 2.5f * s), brush, stroke * 0.94f, roundStroke.Get());
            auto subBrush = makeSubBrush(0.32f);
            const float cell = 4.1f * s;
            const float startX = cx - 6.15f * s;
            const float startY = cy - 6.15f * s;
            for (int rIdx = 0; rIdx < 3; ++rIdx) {
                for (int cIdx = 0; cIdx < 3; ++cIdx) {
                    const float lx = startX + cIdx * cell;
                    const float ty = startY + rIdx * cell;
                    auto tile = D2D1::RectF(lx + 0.25f * s, ty + 0.25f * s, lx + cell - 0.25f * s, ty + cell - 0.25f * s);
                    if ((rIdx + cIdx) % 2 == 0) {
                        rt->FillRoundedRectangle(D2D1::RoundedRect(tile, 0.7f * s, 0.7f * s), brush);
                    } else if (subBrush) {
                        rt->FillRoundedRectangle(D2D1::RoundedRect(tile, 0.7f * s, 0.7f * s), subBrush.Get());
                    }
                }
            }
            break;
        }

        // ── 8.5. 模糊滤镜 (真贝塞尔水滴 + 内侧折射弧) ──────────────────────────────
        case CaptureIconId::ToolBlur: {
            if (factory) {
                Microsoft::WRL::ComPtr<ID2D1PathGeometry> geo;
                if (SUCCEEDED(factory->CreatePathGeometry(geo.GetAddressOf()))) {
                    Microsoft::WRL::ComPtr<ID2D1GeometrySink> sink;
                    if (SUCCEEDED(geo->Open(sink.GetAddressOf()))) {
                        sink->BeginFigure(p(12.0f, 3.0f), D2D1_FIGURE_BEGIN_HOLLOW);
                        sink->AddBezier(D2D1::BezierSegment(p(15.5f, 7.2f), p(18.5f, 10.8f), p(18.5f, 14.2f)));
                        sink->AddArc(D2D1::ArcSegment(
                            p(5.5f, 14.2f),
                            D2D1::SizeF(6.5f * s, 6.5f * s),
                            0.0f,
                            D2D1_SWEEP_DIRECTION_CLOCKWISE,
                            D2D1_ARC_SIZE_SMALL
                        ));
                        sink->AddBezier(D2D1::BezierSegment(p(5.5f, 10.8f), p(8.5f, 7.2f), p(12.0f, 3.0f)));
                        sink->EndFigure(D2D1_FIGURE_END_CLOSED);
                        sink->Close();
                        rt->DrawGeometry(geo.Get(), brush, stroke, roundStroke.Get());
                        drawCircularArc(p(12.0f, 14.2f), 3.6f * s, 105.0f, 65.0f, stroke * 0.8f);
                        break;
                    }
                }
            }
            drawPolyline({ p(12.0f, 3.2f), p(18.0f, 13.5f), p(15.5f, 19.2f), p(12.0f, 20.5f), p(8.5f, 19.2f), p(6.0f, 13.5f) }, true, stroke);
            break;
        }

        // ── 9. 文字 T (高辨识度现代几何双翼衬线 T) ─────────────────────────────────
        case CaptureIconId::ToolText: {
            drawPolyline({ p(4.8f, 7.2f), p(4.8f, 4.6f), p(19.2f, 4.6f), p(19.2f, 7.2f) }, false, stroke);
            rt->DrawLine(p(12.0f, 4.6f), p(12.0f, 19.4f), brush, stroke * 1.05f, roundStroke.Get());
            rt->DrawLine(p(8.8f, 19.4f), p(15.2f, 19.4f), brush, stroke, roundStroke.Get());
            break;
        }

        // ── 10. 序号标记 (饱满圆环 + 光学居中数字 1) ───────────────────────────────
        case CaptureIconId::ToolNumber: {
            auto ellipse = D2D1::Ellipse(D2D1::Point2F(cx, cy), 8.4f * s, 8.4f * s);
            rt->DrawEllipse(ellipse, brush, stroke, roundStroke.Get());
            drawPolyline({ p(10.0f, 9.4f), p(12.2f, 7.6f), p(12.2f, 16.2f) }, false, stroke);
            rt->DrawLine(p(9.8f, 16.2f), p(14.6f, 16.2f), brush, stroke, roundStroke.Get());
            break;
        }

        // ── 10.5. 局部放大镜 (光学透镜圆环 + 镜内精致十字 + 45° 圆润握柄) ─────────
        case CaptureIconId::ToolMagnifier: {
            const D2D1_POINT_2F lensPt = p(10.5f, 10.5f);
            rt->DrawEllipse(D2D1::Ellipse(lensPt, 6.7f * s, 6.7f * s), brush, stroke, roundStroke.Get());
            rt->DrawLine(p(15.4f, 15.4f), p(20.2f, 20.2f), brush, stroke * 1.18f, roundStroke.Get());
            rt->DrawLine(p(8.0f, 10.5f), p(13.0f, 10.5f), brush, stroke * 0.86f, roundStroke.Get());
            rt->DrawLine(p(10.5f, 8.0f), p(10.5f, 13.0f), brush, stroke * 0.86f, roundStroke.Get());
            break;
        }

        // ── 11. 智能消除 (分段镂空魔棒 + 实心四角星芒) ─────────────────────────────
        case CaptureIconId::ToolInpaint: {
            rt->DrawLine(p(3.8f, 20.2f), p(11.0f, 13.0f), brush, stroke * 1.10f, roundStroke.Get());
            rt->DrawLine(p(13.2f, 10.8f), p(14.8f, 9.2f), brush, stroke * 1.10f, roundStroke.Get());
            auto star = createPath({
                p(18.2f, 2.4f), p(19.1f, 5.0f), p(21.7f, 5.9f), p(19.1f, 6.8f),
                p(18.2f, 9.4f), p(17.3f, 6.8f), p(14.7f, 5.9f), p(17.3f, 5.0f)
            }, true);
            if (star) {
                rt->FillGeometry(star.Get(), brush);
            } else {
                rt->DrawLine(p(18.2f, 2.5f), p(18.2f, 9.0f), brush, stroke * 0.9f, roundStroke.Get());
                rt->DrawLine(p(15.0f, 5.8f), p(21.5f, 5.8f), brush, stroke * 0.9f, roundStroke.Get());
            }
            rt->DrawLine(p(8.2f, 3.8f), p(8.2f, 7.2f), brush, stroke * 0.80f, roundStroke.Get());
            rt->DrawLine(p(6.5f, 5.5f), p(9.9f, 5.5f), brush, stroke * 0.80f, roundStroke.Get());
            rt->DrawLine(p(18.5f, 14.0f), p(18.5f, 17.4f), brush, stroke * 0.80f, roundStroke.Get());
            rt->DrawLine(p(16.8f, 15.7f), p(20.2f, 15.7f), brush, stroke * 0.80f, roundStroke.Get());
            break;
        }

        // ── 12. 撤销 (真圆弧回折路径) ──────────────────────────────────────────────
        case CaptureIconId::ActionUndo: {
            drawPolyline({ p(8.8f, 5.0f), p(4.2f, 9.6f), p(8.8f, 14.2f) }, false, stroke);
            if (factory) {
                Microsoft::WRL::ComPtr<ID2D1PathGeometry> geo;
                if (SUCCEEDED(factory->CreatePathGeometry(geo.GetAddressOf()))) {
                    Microsoft::WRL::ComPtr<ID2D1GeometrySink> sink;
                    if (SUCCEEDED(geo->Open(sink.GetAddressOf()))) {
                        sink->BeginFigure(p(4.4f, 9.6f), D2D1_FIGURE_BEGIN_HOLLOW);
                        sink->AddLine(p(14.2f, 9.6f));
                        sink->AddArc(D2D1::ArcSegment(
                            p(14.2f, 19.2f),
                            D2D1::SizeF(4.8f * s, 4.8f * s),
                            0.0f,
                            D2D1_SWEEP_DIRECTION_CLOCKWISE,
                            D2D1_ARC_SIZE_SMALL
                        ));
                        sink->AddLine(p(9.2f, 19.2f));
                        sink->EndFigure(D2D1_FIGURE_END_OPEN);
                        sink->Close();
                        rt->DrawGeometry(geo.Get(), brush, stroke, roundStroke.Get());
                        break;
                    }
                }
            }
            rt->DrawLine(p(4.4f, 9.6f), p(14.2f, 9.6f), brush, stroke, roundStroke.Get());
            drawCircularArc(p(14.2f, 14.4f), 4.8f * s, -90.0f, 180.0f, stroke);
            rt->DrawLine(p(14.2f, 19.2f), p(9.2f, 19.2f), brush, stroke, roundStroke.Get());
            break;
        }

        // ── 13. 重做 (真圆弧回折路径) ──────────────────────────────────────────────
        case CaptureIconId::ActionRedo: {
            drawPolyline({ p(15.2f, 5.0f), p(19.8f, 9.6f), p(15.2f, 14.2f) }, false, stroke);
            if (factory) {
                Microsoft::WRL::ComPtr<ID2D1PathGeometry> geo;
                if (SUCCEEDED(factory->CreatePathGeometry(geo.GetAddressOf()))) {
                    Microsoft::WRL::ComPtr<ID2D1GeometrySink> sink;
                    if (SUCCEEDED(geo->Open(sink.GetAddressOf()))) {
                        sink->BeginFigure(p(19.6f, 9.6f), D2D1_FIGURE_BEGIN_HOLLOW);
                        sink->AddLine(p(9.8f, 9.6f));
                        sink->AddArc(D2D1::ArcSegment(
                            p(9.8f, 19.2f),
                            D2D1::SizeF(4.8f * s, 4.8f * s),
                            0.0f,
                            D2D1_SWEEP_DIRECTION_COUNTER_CLOCKWISE,
                            D2D1_ARC_SIZE_SMALL
                        ));
                        sink->AddLine(p(14.8f, 19.2f));
                        sink->EndFigure(D2D1_FIGURE_END_OPEN);
                        sink->Close();
                        rt->DrawGeometry(geo.Get(), brush, stroke, roundStroke.Get());
                        break;
                    }
                }
            }
            rt->DrawLine(p(19.6f, 9.6f), p(9.8f, 9.6f), brush, stroke, roundStroke.Get());
            drawCircularArc(p(9.8f, 14.4f), 4.8f * s, -90.0f, -180.0f, stroke);
            rt->DrawLine(p(9.8f, 19.2f), p(14.8f, 19.2f), brush, stroke, roundStroke.Get());
            break;
        }

        // ── 14. 清空 / 垃圾桶 ──────────────────────────────────────────────────────
        case CaptureIconId::ActionClear: {
            rt->DrawLine(p(3.5f, 6.2f), p(20.5f, 6.2f), brush, stroke, roundStroke.Get());
            drawPolyline({ p(8.5f, 6.2f), p(8.5f, 3.8f), p(15.5f, 3.8f), p(15.5f, 6.2f) }, false, stroke);
            auto bin = D2D1::RectF(cx - 6.2f * s, cy - 5.8f * s, cx + 6.2f * s, cy + 8.5f * s);
            rt->DrawRoundedRectangle(D2D1::RoundedRect(bin, 2.0f * s, 2.0f * s), brush, stroke, roundStroke.Get());
            rt->DrawLine(p(10.0f, 10.2f), p(10.0f, 16.2f), brush, stroke * 0.9f, roundStroke.Get());
            rt->DrawLine(p(14.0f, 10.2f), p(14.0f, 16.2f), brush, stroke * 0.9f, roundStroke.Get());
            break;
        }

        // ── 15. OCR 文本提取 (四角取景框 + 错落文本行) ─────────────────────────────
        case CaptureIconId::ActionExtractText: {
            drawPolyline({ p(4.0f, 8.0f), p(4.0f, 4.2f), p(7.8f, 4.2f) }, false, stroke);
            drawPolyline({ p(16.2f, 4.2f), p(20.0f, 4.2f), p(20.0f, 8.0f) }, false, stroke);
            drawPolyline({ p(4.0f, 16.0f), p(4.0f, 19.8f), p(7.8f, 19.8f) }, false, stroke);
            drawPolyline({ p(16.2f, 19.8f), p(20.0f, 19.8f), p(20.0f, 16.0f) }, false, stroke);
            rt->DrawLine(p(7.6f, 9.0f), p(16.4f, 9.0f), brush, stroke * 0.92f, roundStroke.Get());
            rt->DrawLine(p(7.6f, 12.2f), p(16.4f, 12.2f), brush, stroke * 0.92f, roundStroke.Get());
            rt->DrawLine(p(7.6f, 15.4f), p(13.2f, 15.4f), brush, stroke * 0.92f, roundStroke.Get());
            break;
        }

        // ── 16. 钉在桌面 (优雅现代斜置/垂直图钉) ───────────────────────────────────
        case CaptureIconId::ActionPinWindow: {
            drawPolyline({
                p(8.6f, 4.0f),
                p(15.4f, 4.0f),
                p(15.4f, 9.4f),
                p(18.0f, 13.6f),
                p(6.0f, 13.6f),
                p(8.6f, 9.4f)
            }, true, stroke);
            rt->DrawLine(p(7.4f, 4.0f), p(16.6f, 4.0f), brush, stroke, roundStroke.Get());
            rt->DrawLine(p(12.0f, 13.6f), p(12.0f, 20.4f), brush, stroke, roundStroke.Get());
            break;
        }

        // ── 17. 滚动长截图 (长视口双护翼 + 居中向下滚动箭头 + 底部扩展叠页线) ──────
        case CaptureIconId::ActionScrollCapture: {
            drawPolyline({ p(4.4f, 9.5f), p(4.4f, 4.2f), p(8.2f, 4.2f) }, false, stroke * 0.90f);
            drawPolyline({ p(19.6f, 9.5f), p(19.6f, 4.2f), p(15.8f, 4.2f) }, false, stroke * 0.90f);
            drawPolyline({ p(4.4f, 14.5f), p(4.4f, 19.8f), p(8.2f, 19.8f) }, false, stroke * 0.90f);
            drawPolyline({ p(19.6f, 14.5f), p(19.6f, 19.8f), p(15.8f, 19.8f) }, false, stroke * 0.90f);
            rt->DrawLine(p(12.0f, 6.2f), p(12.0f, 17.2f), brush, stroke, roundStroke.Get());
            drawPolyline({ p(8.2f, 13.6f), p(12.0f, 17.4f), p(15.8f, 13.6f) }, false, stroke);
            break;
        }

        // ── 18. 取消 (克制圆润叉号) ────────────────────────────────────────────────
        case CaptureIconId::ActionCancel: {
            rt->DrawLine(p(17.0f, 7.0f), p(7.0f, 17.0f), brush, stroke * 1.05f, roundStroke.Get());
            rt->DrawLine(p(7.0f, 7.0f), p(17.0f, 17.0f), brush, stroke * 1.05f, roundStroke.Get());
            break;
        }

        // ── 19. 确认 (饱满果断高辨识度对勾) ────────────────────────────────────────
        case CaptureIconId::ActionConfirm: {
            drawPolyline({ p(4.8f, 12.2f), p(9.6f, 17.0f), p(19.4f, 7.0f) }, false, stroke * 1.28f);
            break;
        }

        // ── 19.0 录屏摄影机 ────────────────────────────────────────────────────────
        case CaptureIconId::ActionRecordVideo: {
            auto body = D2D1::RectF(p(3.0f, 6.6f).x, p(3.0f, 6.6f).y, p(14.8f, 17.4f).x, p(14.8f, 17.4f).y);
            rt->DrawRoundedRectangle(D2D1::RoundedRect(body, 2.5f * s, 2.5f * s), brush, stroke, roundStroke.Get());
            drawPolyline({ p(14.8f, 10.2f), p(20.8f, 7.4f), p(20.8f, 16.6f), p(14.8f, 13.8f) }, true, stroke);
            break;
        }

        // ── 19.1 录屏开始 ──────────────────────────────────────────────────────────
        case CaptureIconId::ActionRecordStart: {
            auto circle = D2D1::Ellipse(D2D1::Point2F(cx, cy), 6.5f * s, 6.5f * s);
            rt->FillEllipse(circle, brush);
            break;
        }

        // ── 19.2 录屏暂停 ──────────────────────────────────────────────────────────
        case CaptureIconId::ActionRecordPause: {
            rt->DrawLine(p(9.0f, 6.2f), p(9.0f, 17.8f), brush, stroke * 1.45f, roundStroke.Get());
            rt->DrawLine(p(15.0f, 6.2f), p(15.0f, 17.8f), brush, stroke * 1.45f, roundStroke.Get());
            break;
        }

        // ── 19.3 录屏停止 ──────────────────────────────────────────────────────────
        case CaptureIconId::ActionRecordStop: {
            auto box = D2D1::RectF(cx - 5.5f * s, cy - 5.5f * s, cx + 5.5f * s, cy + 5.5f * s);
            rt->FillRoundedRectangle(D2D1::RoundedRect(box, 2.0f * s, 2.0f * s), brush);
            break;
        }

        // ── 19.4 麦克风 ────────────────────────────────────────────────────────────
        case CaptureIconId::ActionToggleMic: {
            auto micCap = D2D1::RectF(cx - 3.2f * s, cy - 8.5f * s, cx + 3.2f * s, cy + 1.5f * s);
            rt->DrawRoundedRectangle(D2D1::RoundedRect(micCap, 3.2f * s, 3.2f * s), brush, stroke, roundStroke.Get());
            drawCircularArc(p(12.0f, 11.0f), 6.0f * s, 0.0f, 180.0f, stroke);
            rt->DrawLine(p(12.0f, 17.0f), p(12.0f, 20.8f), brush, stroke, roundStroke.Get());
            rt->DrawLine(p(8.2f, 20.8f), p(15.8f, 20.8f), brush, stroke, roundStroke.Get());
            break;
        }

        // ── 19.5 扬声器 ────────────────────────────────────────────────────────────
        case CaptureIconId::ActionToggleSpeaker: {
            drawPolyline({ p(3.8f, 9.2f), p(7.8f, 9.2f), p(12.8f, 5.0f), p(12.8f, 19.0f), p(7.8f, 14.8f), p(3.8f, 14.8f) }, true, stroke);
            drawCircularArc(p(13.2f, 12.0f), 3.8f * s, -45.0f, 90.0f, stroke);
            drawCircularArc(p(13.2f, 12.0f), 7.0f * s, -48.0f, 96.0f, stroke);
            break;
        }

        // ── 19.6 复制 (双层圆角卡片) ───────────────────────────────────────────────
        case CaptureIconId::ActionCopy: {
            auto frontBox = D2D1::RectF(p(4.2f, 8.4f).x, p(4.2f, 8.4f).y, p(15.8f, 20.0f).x, p(15.8f, 20.0f).y);
            rt->DrawRoundedRectangle(D2D1::RoundedRect(frontBox, 2.3f * s, 2.3f * s), brush, stroke, roundStroke.Get());
            if (factory) {
                Microsoft::WRL::ComPtr<ID2D1PathGeometry> geo;
                if (SUCCEEDED(factory->CreatePathGeometry(geo.GetAddressOf()))) {
                    Microsoft::WRL::ComPtr<ID2D1GeometrySink> sink;
                    if (SUCCEEDED(geo->Open(sink.GetAddressOf()))) {
                        sink->BeginFigure(p(8.2f, 5.6f), D2D1_FIGURE_BEGIN_HOLLOW);
                        sink->AddLine(p(8.2f, 5.2f));
                        sink->AddArc(D2D1::ArcSegment(p(10.2f, 3.8f), D2D1::SizeF(2.0f * s, 2.0f * s), 0.0f, D2D1_SWEEP_DIRECTION_CLOCKWISE, D2D1_ARC_SIZE_SMALL));
                        sink->AddLine(p(17.8f, 3.8f));
                        sink->AddArc(D2D1::ArcSegment(p(20.0f, 6.0f), D2D1::SizeF(2.2f * s, 2.2f * s), 0.0f, D2D1_SWEEP_DIRECTION_CLOCKWISE, D2D1_ARC_SIZE_SMALL));
                        sink->AddLine(p(20.0f, 13.8f));
                        sink->AddArc(D2D1::ArcSegment(p(18.5f, 15.8f), D2D1::SizeF(2.0f * s, 2.0f * s), 0.0f, D2D1_SWEEP_DIRECTION_CLOCKWISE, D2D1_ARC_SIZE_SMALL));
                        sink->EndFigure(D2D1_FIGURE_END_OPEN);
                        sink->Close();
                        rt->DrawGeometry(geo.Get(), brush, stroke, roundStroke.Get());
                        break;
                    }
                }
            }
            drawPolyline({ p(8.5f, 5.5f), p(8.5f, 4.0f), p(20.0f, 4.0f), p(20.0f, 15.5f), p(18.5f, 15.5f) }, false, stroke);
            break;
        }

        // ── 19.7 保存 / 下载 ───────────────────────────────────────────────────────
        case CaptureIconId::ActionSave: {
            rt->DrawLine(p(12.0f, 4.0f), p(12.0f, 14.8f), brush, stroke, roundStroke.Get());
            drawPolyline({ p(7.5f, 10.2f), p(12.0f, 14.8f), p(16.5f, 10.2f) }, false, stroke);
            drawPolyline({ p(4.5f, 15.8f), p(4.5f, 19.8f), p(19.5f, 19.8f), p(19.5f, 15.8f) }, false, stroke);
            break;
        }

        // ── 19.8 反色 / 负片 ───────────────────────────────────────────────────────
        case CaptureIconId::ActionInvert: {
            auto circle = D2D1::Ellipse(D2D1::Point2F(cx, cy), 8.6f * s, 8.6f * s);
            rt->DrawEllipse(circle, brush, stroke, roundStroke.Get());
            if (factory) {
                Microsoft::WRL::ComPtr<ID2D1PathGeometry> semi;
                if (SUCCEEDED(factory->CreatePathGeometry(semi.GetAddressOf()))) {
                    Microsoft::WRL::ComPtr<ID2D1GeometrySink> sink;
                    if (SUCCEEDED(semi->Open(sink.GetAddressOf()))) {
                        sink->BeginFigure(D2D1::Point2F(cx, cy - 8.4f * s), D2D1_FIGURE_BEGIN_FILLED);
                        sink->AddArc(D2D1::ArcSegment(
                            D2D1::Point2F(cx, cy + 8.4f * s),
                            D2D1::SizeF(8.4f * s, 8.4f * s),
                            0.0f,
                            D2D1_SWEEP_DIRECTION_CLOCKWISE,
                            D2D1_ARC_SIZE_SMALL
                        ));
                        sink->EndFigure(D2D1_FIGURE_END_CLOSED);
                        sink->Close();
                        rt->FillGeometry(semi.Get(), brush);
                        break;
                    }
                }
            }
            rt->DrawLine(D2D1::Point2F(cx, cy - 8.4f * s), D2D1::Point2F(cx, cy + 8.4f * s), brush, stroke, roundStroke.Get());
            break;
        }

        // ── 19.9 灰度黑白 ──────────────────────────────────────────────────────────
        case CaptureIconId::ActionGrayscale: {
            auto box = D2D1::RectF(cx - 8.2f * s, cy - 8.2f * s, cx + 8.2f * s, cy + 8.2f * s);
            if (auto halfBrush = makeSubBrush(0.36f)) {
                auto rightHalf = D2D1::RectF(cx, cy - 7.8f * s, cx + 7.8f * s, cy + 7.8f * s);
                rt->FillRoundedRectangle(D2D1::RoundedRect(rightHalf, 1.8f * s, 1.8f * s), halfBrush.Get());
            }
            rt->DrawRoundedRectangle(D2D1::RoundedRect(box, 2.4f * s, 2.4f * s), brush, stroke, roundStroke.Get());
            rt->DrawLine(D2D1::Point2F(cx, cy - 8.2f * s), D2D1::Point2F(cx, cy + 8.2f * s), brush, stroke * 0.92f, roundStroke.Get());
            break;
        }

        // ── 19.10 1:1 原始尺寸 ─────────────────────────────────────────────────────
        case CaptureIconId::ActionResetScale: {
            drawPolyline({ p(4.0f, 8.0f), p(4.0f, 4.0f), p(8.0f, 4.0f) }, false, stroke);
            drawPolyline({ p(16.0f, 4.0f), p(20.0f, 4.0f), p(20.0f, 8.0f) }, false, stroke);
            drawPolyline({ p(4.0f, 16.0f), p(4.0f, 20.0f), p(8.0f, 20.0f) }, false, stroke);
            drawPolyline({ p(16.0f, 20.0f), p(20.0f, 20.0f), p(20.0f, 16.0f) }, false, stroke);
            drawPolyline({ p(10.5f, 10.0f), p(12.0f, 8.5f), p(12.0f, 15.5f) }, false, stroke);
            rt->DrawLine(p(10.0f, 15.5f), p(14.0f, 15.5f), brush, stroke, roundStroke.Get());
            break;
        }

        // ── 19.11 折叠贴图 ─────────────────────────────────────────────────────────
        case CaptureIconId::ActionFold: {
            drawPolyline({ p(4.0f, 14.0f), p(10.0f, 14.0f), p(10.0f, 20.0f) }, false, stroke);
            rt->DrawLine(p(10.0f, 14.0f), p(3.5f, 20.5f), brush, stroke, roundStroke.Get());
            drawPolyline({ p(20.0f, 10.0f), p(14.0f, 10.0f), p(14.0f, 4.0f) }, false, stroke);
            rt->DrawLine(p(14.0f, 10.0f), p(20.5f, 3.5f), brush, stroke, roundStroke.Get());
            break;
        }

        // ── 19.11b 旋转贴图 ────────────────────────────────────────────────────────
        case CaptureIconId::ActionRotate: {
            drawCircularArc(D2D1::Point2F(cx, cy), 8.0f * s, 35.0f, 285.0f, stroke);
            drawPolyline({ p(19.5f, 3.8f), p(19.5f, 9.0f), p(14.3f, 9.0f) }, false, stroke);
            break;
        }

        // ── 19.12 重置序号 ─────────────────────────────────────────────────────────
        case CaptureIconId::ActionResetNumber: {
            drawCircularArc(D2D1::Point2F(cx, cy), 8.4f * s, 30.0f, 290.0f, stroke * 0.9f);
            drawPolyline({ p(19.5f, 4.0f), p(19.5f, 8.6f), p(14.9f, 8.6f) }, false, stroke * 0.9f);
            drawPolyline({ p(10.4f, 9.8f), p(12.2f, 8.2f), p(12.2f, 15.8f) }, false, stroke * 0.95f);
            rt->DrawLine(p(10.0f, 15.8f), p(14.4f, 15.8f), brush, stroke * 0.95f, roundStroke.Get());
            break;
        }

        // ── 19.13 录屏高清快照 ─────────────────────────────────────────────────────
        case CaptureIconId::ActionSnapshot: {
            auto body = D2D1::RectF(cx - 8.5f * s, cy - 4.5f * s, cx + 8.5f * s, cy + 7.5f * s);
            rt->DrawRoundedRectangle(D2D1::RoundedRect(body, 2.2f * s, 2.2f * s), brush, stroke, roundStroke.Get());
            rt->DrawEllipse(D2D1::Ellipse(D2D1::Point2F(cx, cy + 1.5f * s), 3.6f * s, 3.6f * s), brush, stroke, roundStroke.Get());
            drawPolyline({ p(8.5f, 7.5f), p(10.0f, 4.8f), p(14.0f, 4.8f), p(15.5f, 7.5f) }, false, stroke);
            break;
        }

        // ── 19.14 打开所在文件夹 ───────────────────────────────────────────────────
        case CaptureIconId::ActionOpenFolder: {
            drawPolyline({ p(3.5f, 6.5f), p(9.0f, 6.5f), p(11.0f, 9.0f), p(20.5f, 9.0f), p(20.5f, 18.5f), p(3.5f, 18.5f) }, true, stroke);
            break;
        }

        // ── 20. 实线样式 ───────────────────────────────────────────────────────────
        case CaptureIconId::PropSolidLine: {
            float lx1 = rect.left + 4.5f * scale;
            float lx2 = rect.right - 4.5f * scale;
            rt->DrawLine(D2D1::Point2F(lx1, cy), D2D1::Point2F(lx2, cy), brush, stroke, roundStroke.Get());
            break;
        }

        // ── 21. 长虚线样式 ─────────────────────────────────────────────────────────
        case CaptureIconId::PropDashedLine: {
            float lx1 = rect.left + 4.0f * scale;
            float lx2 = rect.right - 4.0f * scale;
            float seg = 4.8f * scale, sp = 2.8f * scale;
            for (float sx = lx1; sx < lx2; sx += seg + sp) {
                rt->DrawLine(D2D1::Point2F(sx, cy), D2D1::Point2F((std::min)(lx2, sx + seg), cy), brush, stroke, roundStroke.Get());
            }
            break;
        }

        // ── 22. 点虚线样式 ─────────────────────────────────────────────────────────
        case CaptureIconId::PropDottedLine: {
            float lx1 = rect.left + 4.0f * scale;
            float lx2 = rect.right - 4.0f * scale;
            float seg = 1.6f * scale, sp = 2.6f * scale;
            for (float sx = lx1; sx < lx2; sx += seg + sp) {
                rt->DrawLine(D2D1::Point2F(sx, cy), D2D1::Point2F((std::min)(lx2, sx + seg), cy), brush, stroke, roundStroke.Get());
            }
            break;
        }

        // ── 23. 点划线样式 ─────────────────────────────────────────────────────────
        case CaptureIconId::PropDashDotLine: {
            float lx1 = rect.left + 4.0f * scale;
            float lx2 = rect.right - 4.0f * scale;
            float sx = lx1;
            while (sx < lx2) {
                rt->DrawLine(D2D1::Point2F(sx, cy), D2D1::Point2F((std::min)(lx2, sx + 4.8f * scale), cy), brush, stroke, roundStroke.Get());
                sx += 7.2f * scale;
                if (sx < lx2) {
                    rt->DrawLine(D2D1::Point2F(sx, cy), D2D1::Point2F((std::min)(lx2, sx + 1.6f * scale), cy), brush, stroke, roundStroke.Get());
                    sx += 3.8f * scale;
                }
            }
            break;
        }

        // ── 24. 线宽步进器图标 ─────────────────────────────────────────────────────
        case CaptureIconId::PropStrokeWidth: {
            const float w1 = (std::max)(1.0f * scale, stroke * 0.62f);
            const float w2 = (std::max)(1.4f * scale, stroke * 1.00f);
            const float w3 = (std::max)(1.9f * scale, stroke * 1.42f);
            rt->DrawLine(p(4.0f, 6.0f), p(20.0f, 6.0f), brush, w1, roundStroke.Get());
            rt->DrawLine(p(4.0f, 12.0f), p(20.0f, 12.0f), brush, w2, roundStroke.Get());
            rt->DrawLine(p(4.0f, 18.0f), p(20.0f, 18.0f), brush, w3, roundStroke.Get());
            break;
        }

        // ── 25. 圆角步进器图标（双对比度无缝闭合圆角框：淡色 L 直角基准边 + 主高亮饱满大圆角弧边） ──
        case CaptureIconId::PropCornerRadius: {
            // 1. 次级直角基准 L 基座（交代完整正方形轮廓与左下 90° 标准直角参照，两端留出 0.8*s 防圆帽叠涂重影）
            if (auto baseBrush = makeSubBrush(0.38f)) {
                Microsoft::WRL::ComPtr<ID2D1PathGeometry> baseGeo;
                if (factory && SUCCEEDED(factory->CreatePathGeometry(baseGeo.GetAddressOf()))) {
                    Microsoft::WRL::ComPtr<ID2D1GeometrySink> sink;
                    if (SUCCEEDED(baseGeo->Open(sink.GetAddressOf()))) {
                        sink->BeginFigure(p(4.2f, 5.0f), D2D1_FIGURE_BEGIN_HOLLOW);
                        sink->AddLine(p(4.2f, 19.8f));
                        sink->AddLine(p(19.0f, 19.8f));
                        sink->EndFigure(D2D1_FIGURE_END_OPEN);
                        sink->Close();
                        rt->DrawGeometry(baseGeo.Get(), baseBrush.Get(), stroke * 0.86f, roundStroke.Get());
                    }
                } else {
                    rt->DrawLine(p(4.2f, 5.0f), p(4.2f, 19.8f), baseBrush.Get(), stroke * 0.86f, roundStroke.Get());
                    rt->DrawLine(p(4.2f, 19.8f), p(19.0f, 19.8f), baseBrush.Get(), stroke * 0.86f, roundStroke.Get());
                }
            }
            // 2. 主高亮饱满大圆角边（左上顶点 -> 上横边 -> R=8.6 大曲率正圆弧 -> 右竖边 -> 右下顶点，无缝闭合）
            if (factory) {
                Microsoft::WRL::ComPtr<ID2D1PathGeometry> geo;
                if (SUCCEEDED(factory->CreatePathGeometry(geo.GetAddressOf()))) {
                    Microsoft::WRL::ComPtr<ID2D1GeometrySink> sink;
                    if (SUCCEEDED(geo->Open(sink.GetAddressOf()))) {
                        sink->BeginFigure(p(4.2f, 4.2f), D2D1_FIGURE_BEGIN_HOLLOW);
                        sink->AddLine(p(11.2f, 4.2f));
                        sink->AddArc(D2D1::ArcSegment(
                            p(19.8f, 12.8f),
                            D2D1::SizeF(8.6f * s, 8.6f * s),
                            0.0f,
                            D2D1_SWEEP_DIRECTION_CLOCKWISE,
                            D2D1_ARC_SIZE_SMALL
                        ));
                        sink->AddLine(p(19.8f, 19.8f));
                        sink->EndFigure(D2D1_FIGURE_END_OPEN);
                        sink->Close();
                        rt->DrawGeometry(geo.Get(), brush, stroke * 1.06f, roundStroke.Get());
                        break;
                    }
                }
            }
            drawPolyline({ p(4.2f, 4.2f), p(11.2f, 4.2f), p(17.3f, 6.7f), p(19.8f, 12.8f), p(19.8f, 19.8f) }, false, stroke * 1.06f);
            break;
        }

        // ── 25b. 选区状态重置图标（双弧循环同步箭头，对标 PixPin 右侧第 4 枚重置圆钮） ─────────
        case CaptureIconId::ActionResetSelection: {
            const float r = 6.8f * s;
            if (factory) {
                auto drawHalfLoop = [&](D2D1_POINT_2F a0, D2D1_POINT_2F a1, D2D1_POINT_2F tip1, D2D1_POINT_2F tip2) {
                    Microsoft::WRL::ComPtr<ID2D1PathGeometry> geo;
                    if (SUCCEEDED(factory->CreatePathGeometry(geo.GetAddressOf()))) {
                        Microsoft::WRL::ComPtr<ID2D1GeometrySink> sink;
                        if (SUCCEEDED(geo->Open(sink.GetAddressOf()))) {
                            sink->BeginFigure(a0, D2D1_FIGURE_BEGIN_HOLLOW);
                            sink->AddArc(D2D1::ArcSegment(
                                a1, D2D1::SizeF(r, r), 0.0f,
                                D2D1_SWEEP_DIRECTION_CLOCKWISE, D2D1_ARC_SIZE_SMALL));
                            sink->EndFigure(D2D1_FIGURE_END_OPEN);
                            sink->Close();
                            rt->DrawGeometry(geo.Get(), brush, stroke * 0.96f, roundStroke.Get());
                        }
                    }
                    drawPolyline({ tip1, a1, tip2 }, false, stroke * 0.94f);
                };
                drawHalfLoop(p(6.0f, 9.2f), p(18.0f, 9.2f), p(15.2f, 6.6f), p(15.4f, 11.8f));
                drawHalfLoop(p(18.0f, 14.8f), p(6.0f, 14.8f), p(8.8f, 12.2f), p(8.6f, 17.4f));
                break;
            }
            rt->DrawEllipse(D2D1::Ellipse(D2D1::Point2F(cx, cy), r, r), brush, stroke * 0.95f);
            break;
        }

        // ── 26. 描边模式 ───────────────────────────────────────────────────────────
        case CaptureIconId::PropFillOutline: {
            auto box = D2D1::RectF(cx - 7.5f * s, cy - 7.5f * s, cx + 7.5f * s, cy + 7.5f * s);
            rt->DrawRoundedRectangle(D2D1::RoundedRect(box, 2.2f * s, 2.2f * s), brush, stroke, roundStroke.Get());
            break;
        }

        // ── 27. 实心填充模式 ───────────────────────────────────────────────────────
        case CaptureIconId::PropFillSolid: {
            auto box = D2D1::RectF(cx - 7.5f * s, cy - 7.5f * s, cx + 7.5f * s, cy + 7.5f * s);
            rt->FillRoundedRectangle(D2D1::RoundedRect(box, 2.4f * s, 2.4f * s), brush);
            break;
        }

        // ── 28. 取色吸管 ───────────────────────────────────────────────────────────
        case CaptureIconId::PropPipette: {
            drawPolyline({ p(18.5f, 3.5f), p(20.5f, 5.5f), p(12.0f, 14.0f), p(8.0f, 18.0f), p(3.5f, 20.5f), p(6.0f, 16.0f), p(10.0f, 12.0f) }, true, stroke);
            rt->DrawLine(p(13.5f, 6.5f), p(17.5f, 10.5f), brush, stroke, roundStroke.Get());
            break;
        }

        // ── 29. 调色板 ─────────────────────────────────────────────────────────────
        case CaptureIconId::PropPalette: {
            auto palBox = D2D1::RectF(cx - 8.2f * s, cy - 8.2f * s, cx + 8.2f * s, cy + 8.2f * s);
            rt->DrawRoundedRectangle(D2D1::RoundedRect(palBox, 5.0f * s, 5.0f * s), brush, stroke, roundStroke.Get());
            static const D2D1_COLOR_F palPigments[4] = {
                D2D1::ColorF(0.96f, 0.25f, 0.37f, 1.0f),
                D2D1::ColorF(0.96f, 0.62f, 0.04f, 1.0f),
                D2D1::ColorF(0.06f, 0.73f, 0.51f, 1.0f),
                D2D1::ColorF(0.23f, 0.51f, 0.96f, 1.0f),
            };
            const D2D1_POINT_2F holePts[4] = { p(8.5f, 8.5f), p(15.5f, 8.5f), p(8.5f, 15.5f), p(15.5f, 15.5f) };
            for (int i = 0; i < 4; ++i) {
                Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> pigmentBrush;
                rt->CreateSolidColorBrush(palPigments[i], pigmentBrush.GetAddressOf());
                if (pigmentBrush) {
                    rt->FillEllipse(D2D1::Ellipse(holePts[i], 1.6f * s, 1.6f * s), pigmentBrush.Get());
                }
            }
            break;
        }

        // ── 29.1 全色域彩虹拾色环 ──────────────────────────────────────────────────
        case CaptureIconId::PropRainbowWheel: {
            static const D2D1_COLOR_F spectrumColors[8] = {
                D2D1::ColorF(0.96f, 0.25f, 0.37f, 1.0f),
                D2D1::ColorF(0.96f, 0.62f, 0.04f, 1.0f),
                D2D1::ColorF(0.92f, 0.70f, 0.03f, 1.0f),
                D2D1::ColorF(0.06f, 0.73f, 0.51f, 1.0f),
                D2D1::ColorF(0.02f, 0.71f, 0.83f, 1.0f),
                D2D1::ColorF(0.23f, 0.51f, 0.96f, 1.0f),
                D2D1::ColorF(0.66f, 0.33f, 0.97f, 1.0f),
                D2D1::ColorF(0.93f, 0.28f, 0.60f, 1.0f)
            };

            const float ringR = 6.8f * s;
            const float dotR = 2.0f * s;
            for (int i = 0; i < 8; ++i) {
                float angle = i * (3.14159265f / 4.0f) - 3.14159265f * 0.5f;
                float dotX = cx + ringR * std::cos(angle);
                float dotY = cy + ringR * std::sin(angle);
                Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> dotBrush;
                rt->CreateSolidColorBrush(spectrumColors[i], dotBrush.GetAddressOf());
                if (dotBrush) {
                    rt->FillEllipse(D2D1::Ellipse(D2D1::Point2F(dotX, dotY), dotR, dotR), dotBrush.Get());
                }
            }
            const float crossLen = 2.2f * s;
            const float crossStroke = (std::max)(1.1f * scale, stroke * 0.75f);
            rt->DrawLine(D2D1::Point2F(cx - crossLen, cy), D2D1::Point2F(cx + crossLen, cy), brush, crossStroke, roundStroke.Get());
            rt->DrawLine(D2D1::Point2F(cx, cy - crossLen), D2D1::Point2F(cx, cy + crossLen), brush, crossStroke, roundStroke.Get());
            break;
        }

        // ── 30. 二维码 ─────────────────────────────────────────────────────────────
        case CaptureIconId::PropQrCode: {
            rt->DrawRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(p(3.5f, 3.5f).x, p(3.5f, 3.5f).y, p(9.5f, 9.5f).x, p(9.5f, 9.5f).y), 1.2f * s, 1.2f * s), brush, stroke * 0.9f, roundStroke.Get());
            rt->FillRectangle(D2D1::RectF(p(5.5f, 5.5f).x, p(5.5f, 5.5f).y, p(7.5f, 7.5f).x, p(7.5f, 7.5f).y), brush);
            rt->DrawRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(p(14.5f, 3.5f).x, p(14.5f, 3.5f).y, p(20.5f, 9.5f).x, p(20.5f, 9.5f).y), 1.2f * s, 1.2f * s), brush, stroke * 0.9f, roundStroke.Get());
            rt->FillRectangle(D2D1::RectF(p(16.5f, 5.5f).x, p(16.5f, 5.5f).y, p(18.5f, 7.5f).x, p(18.5f, 7.5f).y), brush);
            rt->DrawRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(p(3.5f, 14.5f).x, p(3.5f, 14.5f).y, p(9.5f, 20.5f).x, p(9.5f, 20.5f).y), 1.2f * s, 1.2f * s), brush, stroke * 0.9f, roundStroke.Get());
            rt->FillRectangle(D2D1::RectF(p(5.5f, 16.5f).x, p(5.5f, 16.5f).y, p(7.5f, 18.5f).x, p(7.5f, 18.5f).y), brush);
            rt->FillRectangle(D2D1::RectF(p(14.5f, 14.5f).x, p(14.5f, 14.5f).y, p(16.8f, 16.8f).x, p(16.8f, 16.8f).y), brush);
            rt->FillRectangle(D2D1::RectF(p(18.2f, 14.5f).x, p(18.2f, 14.5f).y, p(20.5f, 16.8f).x, p(20.5f, 16.8f).y), brush);
            rt->FillRectangle(D2D1::RectF(p(14.5f, 18.2f).x, p(14.5f, 18.2f).y, p(16.8f, 20.5f).x, p(16.8f, 20.5f).y), brush);
            rt->FillRectangle(D2D1::RectF(p(18.2f, 18.2f).x, p(18.2f, 18.2f).y, p(20.5f, 20.5f).x, p(20.5f, 20.5f).y), brush);
            break;
        }

        // ── 31. 美化外壳 (外层画廊装裱圆角框 + 内层悬浮截图卡片 + 右上精致星芒) ────
        case CaptureIconId::ToolBeautyShell: {
            auto outerBox = D2D1::RectF(p(3.0f, 4.2f).x, p(3.0f, 4.2f).y, p(21.0f, 19.8f).x, p(21.0f, 19.8f).y);
            rt->DrawRoundedRectangle(D2D1::RoundedRect(outerBox, 3.0f * s, 3.0f * s), brush, stroke * 0.92f, roundStroke.Get());
            if (auto subFill = makeSubBrush(0.22f)) {
                auto innerCard = D2D1::RectF(p(6.4f, 7.6f).x, p(6.4f, 7.6f).y, p(17.6f, 16.4f).x, p(17.6f, 16.4f).y);
                rt->FillRoundedRectangle(D2D1::RoundedRect(innerCard, 1.6f * s, 1.6f * s), subFill.Get());
            }
            auto innerCard = D2D1::RectF(p(6.4f, 7.6f).x, p(6.4f, 7.6f).y, p(17.6f, 16.4f).x, p(17.6f, 16.4f).y);
            rt->DrawRoundedRectangle(D2D1::RoundedRect(innerCard, 1.6f * s, 1.6f * s), brush, stroke * 0.85f, roundStroke.Get());
            auto star = createPath({
                p(17.6f, 4.2f), p(18.2f, 6.0f), p(20.0f, 6.6f), p(18.2f, 7.2f),
                p(17.6f, 9.0f), p(17.0f, 7.2f), p(15.2f, 6.6f), p(17.0f, 6.0f)
            }, true);
            if (star) {
                rt->FillGeometry(star.Get(), brush);
            }
            break;
        }

        // ── 32. 锁定选区比例 ───────────────────────────────────────────────────────
        case CaptureIconId::PropAspectRatio: {
            auto frameBox = D2D1::RectF(p(3.0f, 5.5f).x, p(3.0f, 5.5f).y, p(21.0f, 18.5f).x, p(21.0f, 18.5f).y);
            rt->DrawRoundedRectangle(D2D1::RoundedRect(frameBox, 2.2f * s, 2.2f * s), brush, stroke, roundStroke.Get());
            drawPolyline({ p(6.5f, 11.5f), p(6.5f, 8.5f), p(9.5f, 8.5f) }, false, stroke * 0.9f);
            drawPolyline({ p(17.5f, 12.5f), p(17.5f, 15.5f), p(14.5f, 15.5f) }, false, stroke * 0.9f);
            break;
        }

        // ── 33. 方角序号微晶胶囊 ───────────────────────────────────────────────────
        case CaptureIconId::PropNumberSquare: {
            auto sqBox = D2D1::RectF(p(3.8f, 3.8f).x, p(3.8f, 3.8f).y, p(20.2f, 20.2f).x, p(20.2f, 20.2f).y);
            rt->DrawRoundedRectangle(D2D1::RoundedRect(sqBox, 4.0f * s, 4.0f * s), brush, stroke, roundStroke.Get());
            drawPolyline({ p(9.8f, 9.2f), p(12.2f, 7.4f), p(12.2f, 16.4f) }, false, stroke);
            rt->DrawLine(p(9.6f, 16.4f), p(14.8f, 16.4f), brush, stroke, roundStroke.Get());
            break;
        }

        // ── 33.1 圆角光学方镜 (局部放大镜方形透镜切换图标) ─────────────────────────
        case CaptureIconId::PropMagnifierSquare: {
            auto sqLens = D2D1::RectF(p(3.8f, 3.8f).x, p(3.8f, 3.8f).y, p(16.8f, 16.8f).x, p(16.8f, 16.8f).y);
            rt->DrawRoundedRectangle(D2D1::RoundedRect(sqLens, 3.4f * s, 3.4f * s), brush, stroke, roundStroke.Get());
            rt->DrawLine(p(16.2f, 16.2f), p(20.4f, 20.4f), brush, stroke * 1.15f, roundStroke.Get());
            rt->DrawLine(p(7.8f, 10.3f), p(12.8f, 10.3f), brush, stroke * 0.85f, roundStroke.Get());
            rt->DrawLine(p(10.3f, 7.8f), p(10.3f, 12.8f), brush, stroke * 0.85f, roundStroke.Get());
            break;
        }

        // ── 33.2 局部放大倍率微标显隐图标 (透镜 + 底部实心胶囊微标) ────────────────
        case CaptureIconId::PropMagnifierBadge: {
            rt->DrawEllipse(D2D1::Ellipse(D2D1::Point2F(cx, cy - 0.8f * s), 7.6f * s, 7.6f * s), brush, stroke * 0.92f, roundStroke.Get());
            auto badge = D2D1::RectF(cx - 5.2f * s, cy + 2.8f * s, cx + 5.2f * s, cy + 7.2f * s);
            rt->FillRoundedRectangle(D2D1::RoundedRect(badge, 2.2f * s, 2.2f * s), brush);
            break;
        }

        // ── 33.3 引出式详图放大模式图标 (左下取景小环 + 单轴精密宝石引线 + 右上大透镜) ─
        case CaptureIconId::PropMagnifierCallout: {
            const auto srcPt = p(5.4f, 18.4f);
            const auto dstPt = p(15.2f, 8.8f);
            const float srcR = 2.4f * s;
            const float dstR = 5.6f * s;
            if (auto bridgeBrush = makeSubBrush(0.72f)) {
                rt->DrawLine(p(7.1f, 16.7f), p(11.2f, 12.7f), bridgeBrush.Get(), stroke * 0.92f, roundStroke.Get());
            }
            rt->FillEllipse(D2D1::Ellipse(p(11.2f, 12.7f), 1.05f * s, 1.05f * s), brush);
            rt->DrawEllipse(D2D1::Ellipse(srcPt, srcR, srcR), brush, stroke * 0.90f, roundStroke.Get());
            rt->DrawEllipse(D2D1::Ellipse(dstPt, dstR, dstR), brush, stroke * 1.05f, roundStroke.Get());
            rt->DrawLine(p(13.5f, 8.8f), p(16.9f, 8.8f), brush, stroke * 0.78f, roundStroke.Get());
            rt->DrawLine(p(15.2f, 7.1f), p(15.2f, 10.5f), brush, stroke * 0.78f, roundStroke.Get());
            break;
        }

        // ── 34. 文字描边 ───────────────────────────────────────────────────────────
        case CaptureIconId::PropTextOutline: {
            drawPolyline({ p(6.2f, 16.2f), p(12.0f, 4.2f), p(17.8f, 16.2f) }, false, stroke * 1.05f);
            rt->DrawLine(p(8.2f, 12.4f), p(15.8f, 12.4f), brush, stroke * 0.95f, roundStroke.Get());
            auto bar = D2D1::RectF(p(4.6f, 18.8f).x, p(4.6f, 18.8f).y, p(19.4f, 20.6f).x, p(19.4f, 20.6f).y);
            rt->FillRoundedRectangle(D2D1::RoundedRect(bar, 0.9f * s, 0.9f * s), brush);
            break;
        }

        // ── 35. 字号大小 ───────────────────────────────────────────────────────────
        case CaptureIconId::PropFontSize: {
            rt->DrawLine(p(3.0f, 5.8f), p(13.2f, 5.8f), brush, stroke, roundStroke.Get());
            rt->DrawLine(p(8.1f, 5.8f), p(8.1f, 18.6f), brush, stroke, roundStroke.Get());
            rt->DrawLine(p(13.8f, 11.2f), p(21.0f, 11.2f), brush, stroke * 0.9f, roundStroke.Get());
            rt->DrawLine(p(17.4f, 11.2f), p(17.4f, 18.6f), brush, stroke * 0.9f, roundStroke.Get());
            break;
        }

        // ── 36. 文字背景胶囊 ───────────────────────────────────────────────────────
        case CaptureIconId::PropTextBackground: {
            auto pill = D2D1::RectF(p(3.0f, 4.4f).x, p(3.0f, 4.4f).y, p(21.0f, 19.6f).x, p(21.0f, 19.6f).y);
            if (auto fillBrush = makeSubBrush(0.22f)) {
                rt->FillRoundedRectangle(D2D1::RoundedRect(pill, 4.0f * s, 4.0f * s), fillBrush.Get());
            }
            rt->DrawRoundedRectangle(D2D1::RoundedRect(pill, 4.0f * s, 4.0f * s), brush, stroke * 0.92f, roundStroke.Get());
            drawPolyline({ p(8.0f, 16.0f), p(12.0f, 7.6f), p(16.0f, 16.0f) }, false, stroke * 0.95f);
            rt->DrawLine(p(9.3f, 13.4f), p(14.7f, 13.4f), brush, stroke * 0.88f, roundStroke.Get());
            break;
        }

        // ── 37. 反选区域 ───────────────────────────────────────────────────────────
        case CaptureIconId::ActionInvertSelection: {
            drawPolyline({ p(3.8f, 8.0f), p(3.8f, 3.8f), p(8.0f, 3.8f) }, false, stroke);
            drawPolyline({ p(16.0f, 3.8f), p(20.2f, 3.8f), p(20.2f, 8.0f) }, false, stroke);
            drawPolyline({ p(3.8f, 16.0f), p(3.8f, 20.2f), p(8.0f, 20.2f) }, false, stroke);
            drawPolyline({ p(16.0f, 20.2f), p(20.2f, 20.2f), p(20.2f, 16.0f) }, false, stroke);
            auto inner = D2D1::RectF(p(8.2f, 8.2f).x, p(8.2f, 8.2f).y, p(15.8f, 15.8f).x, p(15.8f, 15.8f).y);
            rt->FillRoundedRectangle(D2D1::RoundedRect(inner, 1.5f * s, 1.5f * s), brush);
            break;
        }

        // ── 38. 聚光灯工具（弱化外围暗场视窗 + 中心纯净高亮聚光圆环，零毛刺粘连） ──────────────
        case CaptureIconId::ToolSpotlight: {
            if (auto dimBrush = makeSubBrush(0.38f)) {
                auto outerFrame = D2D1::RectF(p(3.2f, 4.2f).x, p(3.2f, 4.2f).y, p(20.8f, 19.8f).x, p(20.8f, 19.8f).y);
                rt->DrawRoundedRectangle(D2D1::RoundedRect(outerFrame, 2.8f * s, 2.8f * s), dimBrush.Get(), stroke * 0.86f, roundStroke.Get());
            }
            if (auto coreBrush = makeSubBrush(0.22f)) {
                rt->FillEllipse(D2D1::Ellipse(p(12.0f, 12.0f), 3.6f * s, 3.6f * s), coreBrush.Get());
            }
            rt->DrawEllipse(D2D1::Ellipse(p(12.0f, 12.0f), 4.0f * s, 4.0f * s), brush, stroke * 1.05f, roundStroke.Get());
            break;
        }

        // ── 39. 贴图编组与按组查看（双层叠放卡片 + 分组徽章圆点） ────────────────────
        case CaptureIconId::ActionPinGroup: {
            if (auto backBrush = makeSubBrush(0.58f)) {
                drawPolyline({ p(8.5f, 4.5f), p(19.8f, 4.5f), p(19.8f, 15.2f) }, false, stroke * 0.92f);
            }
            auto frontCard = D2D1::RectF(p(4.0f, 7.8f).x, p(4.0f, 7.8f).y, p(16.8f, 19.8f).x, p(16.8f, 19.8f).y);
            rt->DrawRoundedRectangle(D2D1::RoundedRect(frontCard, 2.2f * s, 2.2f * s), brush, stroke, roundStroke.Get());
            rt->FillEllipse(D2D1::Ellipse(p(8.2f, 12.0f), 1.65f * s, 1.65f * s), brush);
            rt->DrawLine(p(11.4f, 12.0f), p(14.4f, 12.0f), brush, stroke * 0.88f, roundStroke.Get());
            rt->DrawLine(p(6.8f, 16.0f), p(14.0f, 16.0f), brush, stroke * 0.85f, roundStroke.Get());
            break;
        }

        default:
            break;
    }
}

} // namespace tools3000::capture
