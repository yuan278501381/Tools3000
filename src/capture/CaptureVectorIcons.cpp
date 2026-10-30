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

    // 基于标准 Lucide 24x24 视口网格系统 (中心在 12, 12)，自适应等比缩放
    const float s = (std::min)(btnW, btnH) / 24.0f * 0.72f;
    // 线宽与图标实际视口尺寸成正比，防止窄按钮内线条粘连成黑块，同时保障高分屏锐利度
    const float stroke = std::clamp(s * 2.05f, 1.35f * scale, 1.75f * scale);

    auto p = [&](float x, float y) -> D2D1_POINT_2F {
        return D2D1::Point2F(cx + (x - 12.0f) * s, cy + (y - 12.0f) * s);
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
        // Fallback: 16 段平滑折线近似
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
        // ── 1. 矩形工具 (Lucide: Square) ───────────────────────────────────────────
        case CaptureIconId::ToolRectangle: {
            auto box = D2D1::RectF(cx - 8.2f * s, cy - 8.2f * s, cx + 8.2f * s, cy + 8.2f * s);
            rt->DrawRoundedRectangle(D2D1::RoundedRect(box, 2.5f * s, 2.5f * s), brush, stroke, roundStroke.Get());
            break;
        }

        // ── 1.1 直线工具 (带端点锚点的 45° 矢量线段) ─────────────────────────────────
        case CaptureIconId::ToolLine: {
            rt->DrawLine(p(6.0f, 18.0f), p(18.0f, 6.0f), brush, stroke * 1.05f, roundStroke.Get());
            rt->FillEllipse(D2D1::Ellipse(p(5.0f, 19.0f), 1.7f * s, 1.7f * s), brush);
            rt->FillEllipse(D2D1::Ellipse(p(19.0f, 5.0f), 1.7f * s, 1.7f * s), brush);
            break;
        }

        // ── 2. 椭圆工具 (Lucide: Circle) ────────────────────────────────────────────
        case CaptureIconId::ToolEllipse: {
            auto ellipse = D2D1::Ellipse(D2D1::Point2F(cx, cy), 8.5f * s, 8.5f * s);
            rt->DrawEllipse(ellipse, brush, stroke, roundStroke.Get());
            break;
        }

        // ── 3. 45° 标注箭头 (Lucide: ArrowUpRight) ──────────────────────────────────
        case CaptureIconId::ToolArrow: {
            rt->DrawLine(p(6.0f, 18.0f), p(17.8f, 6.2f), brush, stroke, roundStroke.Get());
            drawPolyline({ p(8.5f, 6.0f), p(18.0f, 6.0f), p(18.0f, 15.5f) }, false, stroke);
            break;
        }

        // ── 3.1 水平标准实心几何单向箭头 (Classic Solid Filled Triangle) ───────────
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

        // ── 4. 水平细线开放箭头 (Lucide: ArrowRight) ────────────────────────────────
        case CaptureIconId::ToolArrowThin: {
            rt->DrawLine(p(4.0f, 12.0f), p(19.2f, 12.0f), brush, stroke, roundStroke.Get());
            drawPolyline({ p(13.5f, 6.5f), p(19.5f, 12.0f), p(13.5f, 17.5f) }, false, stroke);
            break;
        }

        // ── 4.1 水平圆润科技机翼箭头 (Modern Swept Aerodynamic Wing) ────────────────
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

        // ── 5. 水平双向箭头 (Lucide: MoveHorizontal) ────────────────────────────────
        case CaptureIconId::ToolArrowDouble: {
            rt->DrawLine(p(4.8f, 12.0f), p(19.2f, 12.0f), brush, stroke, roundStroke.Get());
            drawPolyline({ p(9.0f, 7.5f), p(4.5f, 12.0f), p(9.0f, 16.5f) }, false, stroke);
            drawPolyline({ p(15.0f, 7.5f), p(19.5f, 12.0f), p(15.0f, 16.5f) }, false, stroke);
            break;
        }

        // ── 6. 铅笔 / 画笔 (Lucide: Pencil - 单闭合几何路径 + 流畅笔迹弧线) ──────────
        case CaptureIconId::ToolPen: {
            drawPolyline({
                p(15.8f, 3.4f),
                p(19.6f, 7.2f),
                p(7.8f, 19.0f),
                p(3.2f, 20.2f),
                p(4.4f, 15.6f)
            }, true, stroke);
            rt->DrawLine(p(13.2f, 6.0f), p(17.0f, 9.8f), brush, stroke * 0.9f, roundStroke.Get());
            rt->DrawLine(p(11.8f, 19.8f), p(19.8f, 19.8f), brush, stroke * 0.95f, roundStroke.Get());
            break;
        }

        // ── 7. 荧光笔 (Lucide: Highlighter - 对称马克笔身 + 楔形笔头 + 半透明荧光带) ─
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
            if (auto* solid = static_cast<ID2D1SolidColorBrush*>(brush)) {
                D2D1_COLOR_F c = solid->GetColor();
                c.a *= 0.35f;
                Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> bandBrush;
                if (SUCCEEDED(rt->CreateSolidColorBrush(c, bandBrush.GetAddressOf())) && bandBrush) {
                    auto swath = D2D1::RectF(p(3.6f, 18.4f).x, p(3.6f, 18.4f).y, p(20.4f, 21.2f).x, p(20.4f, 21.2f).y);
                    rt->FillRoundedRectangle(D2D1::RoundedRect(swath, 1.4f * s, 1.4f * s), bandBrush.Get());
                }
            }
            rt->DrawLine(p(4.0f, 20.2f), p(20.0f, 20.2f), brush, stroke * 1.05f, roundStroke.Get());
            break;
        }

        // ── 8. 马赛克 (3x3 现代像素棋盘格矩阵：充足负空间留白，绝不粘连) ─────────────
        case CaptureIconId::ToolMosaic: {
            auto box = D2D1::RectF(cx - 8.4f * s, cy - 8.4f * s, cx + 8.4f * s, cy + 8.4f * s);
            rt->DrawRoundedRectangle(D2D1::RoundedRect(box, 2.4f * s, 2.4f * s), brush, stroke * 0.92f, roundStroke.Get());
            const float cell = 4.2f * s;
            const float startX = cx - 6.3f * s;
            const float startY = cy - 6.3f * s;
            for (int rIdx = 0; rIdx < 3; ++rIdx) {
                for (int cIdx = 0; cIdx < 3; ++cIdx) {
                    if ((rIdx + cIdx) % 2 == 0) {
                        float lx = startX + cIdx * cell;
                        float ty = startY + rIdx * cell;
                        auto tile = D2D1::RectF(lx + 0.15f * s, ty + 0.15f * s, lx + cell - 0.15f * s, ty + cell - 0.15f * s);
                        rt->FillRoundedRectangle(D2D1::RoundedRect(tile, 0.7f * s, 0.7f * s), brush);
                    }
                }
            }
            break;
        }

        // ── 8.5. 模糊滤镜 (Lucide: Droplet - 真贝塞尔曲线 + 圆弧水滴) ────────────────
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
                        // 水滴内侧柔光弧线
                        drawCircularArc(p(12.0f, 14.2f), 3.6f * s, 105.0f, 65.0f, stroke * 0.8f);
                        break;
                    }
                }
            }
            drawPolyline({ p(12.0f, 3.2f), p(18.0f, 13.5f), p(15.5f, 19.2f), p(12.0f, 20.5f), p(8.5f, 19.2f), p(6.0f, 13.5f) }, true, stroke);
            break;
        }

        // ── 9. 文字 T (Lucide: Type - 经典衬线字体图标) ──────────────────────────────
        case CaptureIconId::ToolText: {
            drawPolyline({ p(4.5f, 7.5f), p(4.5f, 4.5f), p(19.5f, 4.5f), p(19.5f, 7.5f) }, false, stroke);
            rt->DrawLine(p(12.0f, 4.5f), p(12.0f, 19.5f), brush, stroke, roundStroke.Get());
            rt->DrawLine(p(8.5f, 19.5f), p(15.5f, 19.5f), brush, stroke, roundStroke.Get());
            break;
        }

        // ── 10. 序号气泡 ① (Circle + Crisp Numeral 1) ───────────────────────────────
        case CaptureIconId::ToolNumber: {
            auto ellipse = D2D1::Ellipse(D2D1::Point2F(cx, cy), 8.6f * s, 8.6f * s);
            rt->DrawEllipse(ellipse, brush, stroke, roundStroke.Get());
            drawPolyline({ p(9.8f, 9.2f), p(12.2f, 7.4f), p(12.2f, 16.4f) }, false, stroke);
            rt->DrawLine(p(9.6f, 16.4f), p(14.8f, 16.4f), brush, stroke, roundStroke.Get());
            break;
        }

        // ── 11. 智能消除 (Lucide: WandSparkles - 分段镂空魔棒 + 实心四角星芒) ─────────
        case CaptureIconId::ToolInpaint: {
            // 魔棒手柄与发光棒头分段留白，彻底消除单线横刺毛刺感
            rt->DrawLine(p(3.8f, 20.2f), p(11.0f, 13.0f), brush, stroke * 1.12f, roundStroke.Get());
            rt->DrawLine(p(13.2f, 10.8f), p(14.8f, 9.2f), brush, stroke * 1.12f, roundStroke.Get());
            // 右上主四角星芒
            auto star = createPath({
                p(18.2f, 2.2f), p(19.1f, 4.9f), p(21.8f, 5.8f), p(19.1f, 6.7f),
                p(18.2f, 9.4f), p(17.3f, 6.7f), p(14.6f, 5.8f), p(17.3f, 4.9f)
            }, true);
            if (star) {
                rt->FillGeometry(star.Get(), brush);
            } else {
                rt->DrawLine(p(18.2f, 2.5f), p(18.2f, 9.0f), brush, stroke * 0.9f, roundStroke.Get());
                rt->DrawLine(p(15.0f, 5.8f), p(21.5f, 5.8f), brush, stroke * 0.9f, roundStroke.Get());
            }
            // 左上副星光
            rt->DrawLine(p(8.2f, 3.6f), p(8.2f, 7.4f), brush, stroke * 0.82f, roundStroke.Get());
            rt->DrawLine(p(6.3f, 5.5f), p(10.1f, 5.5f), brush, stroke * 0.82f, roundStroke.Get());
            // 右下副星光
            rt->DrawLine(p(18.5f, 13.8f), p(18.5f, 17.6f), brush, stroke * 0.82f, roundStroke.Get());
            rt->DrawLine(p(16.6f, 15.7f), p(20.4f, 15.7f), brush, stroke * 0.82f, roundStroke.Get());
            break;
        }

        // ── 12. 撤销 (Lucide: Undo2 - 真圆弧回折路径) ───────────────────────────────
        case CaptureIconId::ActionUndo: {
            drawPolyline({ p(9.0f, 4.5f), p(4.0f, 9.5f), p(9.0f, 14.5f) }, false, stroke);
            if (factory) {
                Microsoft::WRL::ComPtr<ID2D1PathGeometry> geo;
                if (SUCCEEDED(factory->CreatePathGeometry(geo.GetAddressOf()))) {
                    Microsoft::WRL::ComPtr<ID2D1GeometrySink> sink;
                    if (SUCCEEDED(geo->Open(sink.GetAddressOf()))) {
                        sink->BeginFigure(p(4.2f, 9.5f), D2D1_FIGURE_BEGIN_HOLLOW);
                        sink->AddLine(p(14.2f, 9.5f));
                        sink->AddArc(D2D1::ArcSegment(
                            p(14.2f, 19.5f),
                            D2D1::SizeF(5.0f * s, 5.0f * s),
                            0.0f,
                            D2D1_SWEEP_DIRECTION_CLOCKWISE,
                            D2D1_ARC_SIZE_SMALL
                        ));
                        sink->AddLine(p(9.5f, 19.5f));
                        sink->EndFigure(D2D1_FIGURE_END_OPEN);
                        sink->Close();
                        rt->DrawGeometry(geo.Get(), brush, stroke, roundStroke.Get());
                        break;
                    }
                }
            }
            rt->DrawLine(p(4.2f, 9.5f), p(14.2f, 9.5f), brush, stroke, roundStroke.Get());
            drawCircularArc(p(14.2f, 14.5f), 5.0f * s, -90.0f, 180.0f, stroke);
            rt->DrawLine(p(14.2f, 19.5f), p(9.5f, 19.5f), brush, stroke, roundStroke.Get());
            break;
        }

        // ── 13. 重做 (Lucide: Redo2 - 真圆弧回折路径) ───────────────────────────────
        case CaptureIconId::ActionRedo: {
            drawPolyline({ p(15.0f, 4.5f), p(20.0f, 9.5f), p(15.0f, 14.5f) }, false, stroke);
            if (factory) {
                Microsoft::WRL::ComPtr<ID2D1PathGeometry> geo;
                if (SUCCEEDED(factory->CreatePathGeometry(geo.GetAddressOf()))) {
                    Microsoft::WRL::ComPtr<ID2D1GeometrySink> sink;
                    if (SUCCEEDED(geo->Open(sink.GetAddressOf()))) {
                        sink->BeginFigure(p(19.8f, 9.5f), D2D1_FIGURE_BEGIN_HOLLOW);
                        sink->AddLine(p(9.8f, 9.5f));
                        sink->AddArc(D2D1::ArcSegment(
                            p(9.8f, 19.5f),
                            D2D1::SizeF(5.0f * s, 5.0f * s),
                            0.0f,
                            D2D1_SWEEP_DIRECTION_COUNTER_CLOCKWISE,
                            D2D1_ARC_SIZE_SMALL
                        ));
                        sink->AddLine(p(14.5f, 19.5f));
                        sink->EndFigure(D2D1_FIGURE_END_OPEN);
                        sink->Close();
                        rt->DrawGeometry(geo.Get(), brush, stroke, roundStroke.Get());
                        break;
                    }
                }
            }
            rt->DrawLine(p(19.8f, 9.5f), p(9.8f, 9.5f), brush, stroke, roundStroke.Get());
            drawCircularArc(p(9.8f, 14.5f), 5.0f * s, -90.0f, -180.0f, stroke);
            rt->DrawLine(p(9.8f, 19.5f), p(14.5f, 19.5f), brush, stroke, roundStroke.Get());
            break;
        }

        // ── 14. 清空 / 垃圾桶 (Lucide: Trash2) ───────────────────────────────────────
        case CaptureIconId::ActionClear: {
            rt->DrawLine(p(3.5f, 6.2f), p(20.5f, 6.2f), brush, stroke, roundStroke.Get());
            drawPolyline({ p(8.5f, 6.2f), p(8.5f, 3.8f), p(15.5f, 3.8f), p(15.5f, 6.2f) }, false, stroke);
            auto bin = D2D1::RectF(cx - 6.2f * s, cy - 5.8f * s, cx + 6.2f * s, cy + 8.5f * s);
            rt->DrawRoundedRectangle(D2D1::RoundedRect(bin, 2.0f * s, 2.0f * s), brush, stroke, roundStroke.Get());
            rt->DrawLine(p(10.0f, 10.2f), p(10.0f, 16.2f), brush, stroke * 0.9f, roundStroke.Get());
            rt->DrawLine(p(14.0f, 10.2f), p(14.0f, 16.2f), brush, stroke * 0.9f, roundStroke.Get());
            break;
        }

        // ── 15. OCR 文本提取 (Lucide: ScanText - 四角扫描取景框 + 居中文本纹) ────────
        case CaptureIconId::ActionExtractText: {
            drawPolyline({ p(3.8f, 8.2f), p(3.8f, 4.2f), p(7.8f, 4.2f) }, false, stroke);
            drawPolyline({ p(16.2f, 4.2f), p(20.2f, 4.2f), p(20.2f, 8.2f) }, false, stroke);
            drawPolyline({ p(3.8f, 15.8f), p(3.8f, 19.8f), p(7.8f, 19.8f) }, false, stroke);
            drawPolyline({ p(16.2f, 19.8f), p(20.2f, 19.8f), p(20.2f, 15.8f) }, false, stroke);
            rt->DrawLine(p(7.5f, 8.8f), p(16.5f, 8.8f), brush, stroke * 0.92f, roundStroke.Get());
            rt->DrawLine(p(7.5f, 12.0f), p(16.5f, 12.0f), brush, stroke * 0.92f, roundStroke.Get());
            rt->DrawLine(p(7.5f, 15.2f), p(13.5f, 15.2f), brush, stroke * 0.92f, roundStroke.Get());
            break;
        }

        // ── 16. 钉在桌面 (Lucide: Pin - 一体化图钉轮廓) ──────────────────────────────
        case CaptureIconId::ActionPinWindow: {
            drawPolyline({
                p(8.5f, 3.8f),
                p(15.5f, 3.8f),
                p(15.5f, 9.5f),
                p(18.2f, 13.8f),
                p(5.8f, 13.8f),
                p(8.5f, 9.5f)
            }, true, stroke);
            rt->DrawLine(p(7.2f, 3.8f), p(16.8f, 3.8f), brush, stroke, roundStroke.Get());
            rt->DrawLine(p(12.0f, 13.8f), p(12.0f, 20.8f), brush, stroke, roundStroke.Get());
            break;
        }

        // ── 17. 滚动长截图 (Lucide: ArrowDownToLine + 两侧页面轨) ────────────────────
        case CaptureIconId::ActionScrollCapture: {
            // 两侧虚化页面边轨
            drawPolyline({ p(4.5f, 7.0f), p(4.5f, 4.0f), p(8.0f, 4.0f) }, false, stroke * 0.85f);
            drawPolyline({ p(19.5f, 7.0f), p(19.5f, 4.0f), p(16.0f, 4.0f) }, false, stroke * 0.85f);
            // 中心向下长箭头
            rt->DrawLine(p(12.0f, 4.0f), p(12.0f, 16.5f), brush, stroke, roundStroke.Get());
            drawPolyline({ p(7.5f, 12.0f), p(12.0f, 16.5f), p(16.5f, 12.0f) }, false, stroke);
            // 底部汇总基线
            rt->DrawLine(p(5.0f, 20.2f), p(19.0f, 20.2f), brush, stroke, roundStroke.Get());
            break;
        }

        // ── 18. 取消 (Lucide: X) ────────────────────────────────────────────────────
        case CaptureIconId::ActionCancel: {
            rt->DrawLine(p(17.5f, 6.5f), p(6.5f, 17.5f), brush, stroke * 1.05f, roundStroke.Get());
            rt->DrawLine(p(6.5f, 6.5f), p(17.5f, 17.5f), brush, stroke * 1.05f, roundStroke.Get());
            break;
        }

        // ── 19. 确认 (Lucide: Check) ────────────────────────────────────────────────
        case CaptureIconId::ActionConfirm: {
            drawPolyline({ p(4.5f, 12.2f), p(9.5f, 17.2f), p(19.5f, 6.8f) }, false, stroke * 1.25f);
            break;
        }

        // ── 19.0 录屏摄影机 (Lucide: Video) ─────────────────────────────────────────
        case CaptureIconId::ActionRecordVideo: {
            auto body = D2D1::RectF(p(2.8f, 6.5f).x, p(2.8f, 6.5f).y, p(15.0f, 17.5f).x, p(15.0f, 17.5f).y);
            rt->DrawRoundedRectangle(D2D1::RoundedRect(body, 2.4f * s, 2.4f * s), brush, stroke, roundStroke.Get());
            drawPolyline({ p(15.0f, 10.2f), p(21.2f, 7.2f), p(21.2f, 16.8f), p(15.0f, 13.8f) }, true, stroke);
            break;
        }

        // ── 19.1 录屏开始 (实心高亮圆) ──────────────────────────────────────────────
        case CaptureIconId::ActionRecordStart: {
            auto circle = D2D1::Ellipse(D2D1::Point2F(cx, cy), 6.5f * s, 6.5f * s);
            rt->FillEllipse(circle, brush);
            break;
        }

        // ── 19.2 录屏暂停 (圆角双竖线) ──────────────────────────────────────────────
        case CaptureIconId::ActionRecordPause: {
            rt->DrawLine(p(9.0f, 6.2f), p(9.0f, 17.8f), brush, stroke * 1.45f, roundStroke.Get());
            rt->DrawLine(p(15.0f, 6.2f), p(15.0f, 17.8f), brush, stroke * 1.45f, roundStroke.Get());
            break;
        }

        // ── 19.3 录屏停止 (实心停止方块) ────────────────────────────────────────────
        case CaptureIconId::ActionRecordStop: {
            auto box = D2D1::RectF(cx - 5.5f * s, cy - 5.5f * s, cx + 5.5f * s, cy + 5.5f * s);
            rt->FillRoundedRectangle(D2D1::RoundedRect(box, 2.0f * s, 2.0f * s), brush);
            break;
        }

        // ── 19.4 麦克风 (Lucide: Mic - 真圆弧 U 型拾音托架) ─────────────────────────
        case CaptureIconId::ActionToggleMic: {
            auto micCap = D2D1::RectF(cx - 3.2f * s, cy - 8.5f * s, cx + 3.2f * s, cy + 1.5f * s);
            rt->DrawRoundedRectangle(D2D1::RoundedRect(micCap, 3.2f * s, 3.2f * s), brush, stroke, roundStroke.Get());
            drawCircularArc(p(12.0f, 11.0f), 6.0f * s, 0.0f, 180.0f, stroke);
            rt->DrawLine(p(12.0f, 17.0f), p(12.0f, 20.8f), brush, stroke, roundStroke.Get());
            rt->DrawLine(p(8.2f, 20.8f), p(15.8f, 20.8f), brush, stroke, roundStroke.Get());
            break;
        }

        // ── 19.5 扬声器 (Lucide: Volume2 - 真圆弧声波) ──────────────────────────────
        case CaptureIconId::ActionToggleSpeaker: {
            drawPolyline({ p(3.8f, 9.2f), p(7.8f, 9.2f), p(12.8f, 5.0f), p(12.8f, 19.0f), p(7.8f, 14.8f), p(3.8f, 14.8f) }, true, stroke);
            drawCircularArc(p(13.2f, 12.0f), 3.8f * s, -45.0f, 90.0f, stroke);
            drawCircularArc(p(13.2f, 12.0f), 7.0f * s, -48.0f, 96.0f, stroke);
            break;
        }

        // ── 19.6 复制 (Lucide: Copy - 双层圆角卡片) ──────────────────────────────────
        case CaptureIconId::ActionCopy: {
            auto frontBox = D2D1::RectF(p(4.0f, 8.2f).x, p(4.0f, 8.2f).y, p(15.8f, 20.0f).x, p(15.8f, 20.0f).y);
            rt->DrawRoundedRectangle(D2D1::RoundedRect(frontBox, 2.2f * s, 2.2f * s), brush, stroke, roundStroke.Get());
            if (factory) {
                Microsoft::WRL::ComPtr<ID2D1PathGeometry> geo;
                if (SUCCEEDED(factory->CreatePathGeometry(geo.GetAddressOf()))) {
                    Microsoft::WRL::ComPtr<ID2D1GeometrySink> sink;
                    if (SUCCEEDED(geo->Open(sink.GetAddressOf()))) {
                        sink->BeginFigure(p(8.2f, 5.5f), D2D1_FIGURE_BEGIN_HOLLOW);
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

        // ── 19.7 保存 / 下载 (Lucide: Download) ─────────────────────────────────────
        case CaptureIconId::ActionSave: {
            rt->DrawLine(p(12.0f, 4.0f), p(12.0f, 14.8f), brush, stroke, roundStroke.Get());
            drawPolyline({ p(7.5f, 10.2f), p(12.0f, 14.8f), p(16.5f, 10.2f) }, false, stroke);
            drawPolyline({ p(4.5f, 15.8f), p(4.5f, 19.8f), p(19.5f, 19.8f), p(19.5f, 15.8f) }, false, stroke);
            break;
        }

        // ── 19.8 反色 / 负片 (Lucide: Contrast) ─────────────────────────────────────
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

        // ── 19.9 灰度黑白 (双色阶对半切片图标) ───────────────────────────────────────
        case CaptureIconId::ActionGrayscale: {
            auto box = D2D1::RectF(cx - 8.2f * s, cy - 8.2f * s, cx + 8.2f * s, cy + 8.2f * s);
            if (auto* solid = static_cast<ID2D1SolidColorBrush*>(brush)) {
                D2D1_COLOR_F c = solid->GetColor();
                c.a *= 0.36f;
                Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> halfBrush;
                if (SUCCEEDED(rt->CreateSolidColorBrush(c, halfBrush.GetAddressOf())) && halfBrush) {
                    auto rightHalf = D2D1::RectF(cx, cy - 7.8f * s, cx + 7.8f * s, cy + 7.8f * s);
                    rt->FillRoundedRectangle(D2D1::RoundedRect(rightHalf, 1.8f * s, 1.8f * s), halfBrush.Get());
                }
            }
            rt->DrawRoundedRectangle(D2D1::RoundedRect(box, 2.4f * s, 2.4f * s), brush, stroke, roundStroke.Get());
            rt->DrawLine(D2D1::Point2F(cx, cy - 8.2f * s), D2D1::Point2F(cx, cy + 8.2f * s), brush, stroke * 0.92f, roundStroke.Get());
            break;
        }

        // ── 19.10 1:1 原始尺寸 (四角定焦标 + 1) ─────────────────────────────────────
        case CaptureIconId::ActionResetScale: {
            drawPolyline({ p(4.0f, 8.0f), p(4.0f, 4.0f), p(8.0f, 4.0f) }, false, stroke);
            drawPolyline({ p(16.0f, 4.0f), p(20.0f, 4.0f), p(20.0f, 8.0f) }, false, stroke);
            drawPolyline({ p(4.0f, 16.0f), p(4.0f, 20.0f), p(8.0f, 20.0f) }, false, stroke);
            drawPolyline({ p(16.0f, 20.0f), p(20.0f, 20.0f), p(20.0f, 16.0f) }, false, stroke);
            drawPolyline({ p(10.5f, 10.0f), p(12.0f, 8.5f), p(12.0f, 15.5f) }, false, stroke);
            rt->DrawLine(p(10.0f, 15.5f), p(14.0f, 15.5f), brush, stroke, roundStroke.Get());
            break;
        }

        // ── 19.11 折叠贴图 / 最小化微晶胶囊 (Lucide: Minimize2) ──────────────────────
        case CaptureIconId::ActionFold: {
            drawPolyline({ p(4.0f, 14.0f), p(10.0f, 14.0f), p(10.0f, 20.0f) }, false, stroke);
            rt->DrawLine(p(10.0f, 14.0f), p(3.5f, 20.5f), brush, stroke, roundStroke.Get());
            drawPolyline({ p(20.0f, 10.0f), p(14.0f, 10.0f), p(14.0f, 4.0f) }, false, stroke);
            rt->DrawLine(p(14.0f, 10.0f), p(20.5f, 3.5f), brush, stroke, roundStroke.Get());
            break;
        }

        // ── 19.11b 旋转贴图 (Lucide: RotateCw - 真圆弧顺时针箭头) ────────────────────
        case CaptureIconId::ActionRotate: {
            drawCircularArc(D2D1::Point2F(cx, cy), 8.0f * s, 35.0f, 285.0f, stroke);
            drawPolyline({ p(19.5f, 3.8f), p(19.5f, 9.0f), p(14.3f, 9.0f) }, false, stroke);
            break;
        }

        // ── 19.12 重置序号 (环形旋转箭头 + 中心 1) ───────────────────────────────────
        case CaptureIconId::ActionResetNumber: {
            drawCircularArc(D2D1::Point2F(cx, cy), 8.4f * s, 30.0f, 290.0f, stroke * 0.9f);
            drawPolyline({ p(19.5f, 4.0f), p(19.5f, 8.6f), p(14.9f, 8.6f) }, false, stroke * 0.9f);
            drawPolyline({ p(10.4f, 9.8f), p(12.2f, 8.2f), p(12.2f, 15.8f) }, false, stroke * 0.95f);
            rt->DrawLine(p(10.0f, 15.8f), p(14.4f, 15.8f), brush, stroke * 0.95f, roundStroke.Get());
            break;
        }

        // ── 19.13 录屏高清快照 (Lucide: Camera) ──────────────────────────────────────
        case CaptureIconId::ActionSnapshot: {
            auto body = D2D1::RectF(cx - 8.5f * s, cy - 4.5f * s, cx + 8.5f * s, cy + 7.5f * s);
            rt->DrawRoundedRectangle(D2D1::RoundedRect(body, 2.2f * s, 2.2f * s), brush, stroke, roundStroke.Get());
            rt->DrawEllipse(D2D1::Ellipse(D2D1::Point2F(cx, cy + 1.5f * s), 3.6f * s, 3.6f * s), brush, stroke, roundStroke.Get());
            drawPolyline({ p(8.5f, 7.5f), p(10.0f, 4.8f), p(14.0f, 4.8f), p(15.5f, 7.5f) }, false, stroke);
            break;
        }

        // ── 19.14 打开所在文件夹 (Lucide: Folder) ────────────────────────────────────
        case CaptureIconId::ActionOpenFolder: {
            drawPolyline({ p(3.5f, 6.5f), p(9.0f, 6.5f), p(11.0f, 9.0f), p(20.5f, 9.0f), p(20.5f, 18.5f), p(3.5f, 18.5f) }, true, stroke);
            break;
        }

        // ── 20. 实线样式 ────────────────────────────────────────────────────────────
        case CaptureIconId::PropSolidLine: {
            float lx1 = rect.left + 4.5f * scale;
            float lx2 = rect.right - 4.5f * scale;
            rt->DrawLine(D2D1::Point2F(lx1, cy), D2D1::Point2F(lx2, cy), brush, stroke, roundStroke.Get());
            break;
        }

        // ── 21. 长虚线样式 ──────────────────────────────────────────────────────────
        case CaptureIconId::PropDashedLine: {
            float lx1 = rect.left + 4.0f * scale;
            float lx2 = rect.right - 4.0f * scale;
            float seg = 4.8f * scale, sp = 2.8f * scale;
            for (float sx = lx1; sx < lx2; sx += seg + sp) {
                rt->DrawLine(D2D1::Point2F(sx, cy), D2D1::Point2F((std::min)(lx2, sx + seg), cy), brush, stroke, roundStroke.Get());
            }
            break;
        }

        // ── 22. 点虚线样式 ──────────────────────────────────────────────────────────
        case CaptureIconId::PropDottedLine: {
            float lx1 = rect.left + 4.0f * scale;
            float lx2 = rect.right - 4.0f * scale;
            float seg = 1.6f * scale, sp = 2.6f * scale;
            for (float sx = lx1; sx < lx2; sx += seg + sp) {
                rt->DrawLine(D2D1::Point2F(sx, cy), D2D1::Point2F((std::min)(lx2, sx + seg), cy), brush, stroke, roundStroke.Get());
            }
            break;
        }

        // ── 23. 点划线样式 ──────────────────────────────────────────────────────────
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

        // ── 24. 线宽步进器图标 (三级清晰递进横线，保持充足呼吸间距绝不粘连) ──────────
        case CaptureIconId::PropStrokeWidth: {
            const float w1 = (std::max)(1.0f * scale, stroke * 0.62f);
            const float w2 = (std::max)(1.4f * scale, stroke * 1.00f);
            const float w3 = (std::max)(1.9f * scale, stroke * 1.42f);
            rt->DrawLine(p(4.0f, 6.0f), p(20.0f, 6.0f), brush, w1, roundStroke.Get());
            rt->DrawLine(p(4.0f, 12.0f), p(20.0f, 12.0f), brush, w2, roundStroke.Get());
            rt->DrawLine(p(4.0f, 18.0f), p(20.0f, 18.0f), brush, w3, roundStroke.Get());
            break;
        }

        // ── 25. 圆角步进器图标 (真 90° 圆弧切角 + 焦点圆点) ─────────────────────────
        case CaptureIconId::PropCornerRadius: {
            if (factory) {
                Microsoft::WRL::ComPtr<ID2D1PathGeometry> geo;
                if (SUCCEEDED(factory->CreatePathGeometry(geo.GetAddressOf()))) {
                    Microsoft::WRL::ComPtr<ID2D1GeometrySink> sink;
                    if (SUCCEEDED(geo->Open(sink.GetAddressOf()))) {
                        sink->BeginFigure(p(5.0f, 19.0f), D2D1_FIGURE_BEGIN_HOLLOW);
                        sink->AddLine(p(5.0f, 12.0f));
                        sink->AddArc(D2D1::ArcSegment(
                            p(12.0f, 5.0f),
                            D2D1::SizeF(7.0f * s, 7.0f * s),
                            0.0f,
                            D2D1_SWEEP_DIRECTION_CLOCKWISE,
                            D2D1_ARC_SIZE_SMALL
                        ));
                        sink->AddLine(p(19.0f, 5.0f));
                        sink->EndFigure(D2D1_FIGURE_END_OPEN);
                        sink->Close();
                        rt->DrawGeometry(geo.Get(), brush, stroke, roundStroke.Get());
                        rt->FillEllipse(D2D1::Ellipse(p(13.5f, 13.5f), 1.4f * s, 1.4f * s), brush);
                        break;
                    }
                }
            }
            drawPolyline({ p(5.0f, 19.0f), p(5.0f, 11.5f), p(7.0f, 7.0f), p(11.5f, 5.0f), p(19.0f, 5.0f) }, false, stroke);
            break;
        }

        // ── 26. 描边模式 (双层微晶空心框) ───────────────────────────────────────────
        case CaptureIconId::PropFillOutline: {
            auto box = D2D1::RectF(cx - 7.5f * s, cy - 7.5f * s, cx + 7.5f * s, cy + 7.5f * s);
            rt->DrawRoundedRectangle(D2D1::RoundedRect(box, 2.2f * s, 2.2f * s), brush, stroke, roundStroke.Get());
            break;
        }

        // ── 27. 实心填充模式 (饱满圆角实心块) ───────────────────────────────────────
        case CaptureIconId::PropFillSolid: {
            auto box = D2D1::RectF(cx - 7.5f * s, cy - 7.5f * s, cx + 7.5f * s, cy + 7.5f * s);
            rt->FillRoundedRectangle(D2D1::RoundedRect(box, 2.4f * s, 2.4f * s), brush);
            break;
        }

        // ── 28. 取色吸管 (Lucide: Pipette) ──────────────────────────────────────────
        case CaptureIconId::PropPipette: {
            drawPolyline({ p(18.5f, 3.5f), p(20.5f, 5.5f), p(12.0f, 14.0f), p(8.0f, 18.0f), p(3.5f, 20.5f), p(6.0f, 16.0f), p(10.0f, 12.0f) }, true, stroke);
            rt->DrawLine(p(13.5f, 6.5f), p(17.5f, 10.5f), brush, stroke, roundStroke.Get());
            break;
        }

        // ── 29. 调色板 (Lucide: Palette) ────────────────────────────────────────────
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

        // ── 29.1 全色域彩虹拾色环 (Full Spectrum Rainbow Ring) ─────────────────────
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

        // ── 30. 二维码 (Lucide: QrCode) ─────────────────────────────────────────────
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

        // ── 31. 美化外壳 (macOS/Win11 精致窗口套壳 + 居中四角星芒，彻底杜绝边线碰撞) ─
        case CaptureIconId::ToolBeautyShell: {
            auto outerBox = D2D1::RectF(p(3.0f, 4.2f).x, p(3.0f, 4.2f).y, p(21.0f, 19.8f).x, p(21.0f, 19.8f).y);
            rt->DrawRoundedRectangle(D2D1::RoundedRect(outerBox, 2.6f * s, 2.6f * s), brush, stroke * 0.94f, roundStroke.Get());
            rt->DrawLine(p(3.2f, 8.8f), p(20.8f, 8.8f), brush, stroke * 0.85f, roundStroke.Get());
            const float dotR = (std::max)(0.85f * scale, stroke * 0.48f);
            rt->FillEllipse(D2D1::Ellipse(p(6.4f, 6.5f), dotR, dotR), brush);
            rt->FillEllipse(D2D1::Ellipse(p(9.4f, 6.5f), dotR, dotR), brush);
            auto star = createPath({
                p(12.0f, 10.6f), p(12.85f, 13.35f), p(15.6f, 14.2f), p(12.85f, 15.05f),
                p(12.0f, 17.8f), p(11.15f, 15.05f), p(8.4f, 14.2f), p(11.15f, 13.35f)
            }, true);
            if (star) {
                rt->FillGeometry(star.Get(), brush);
            } else {
                rt->DrawLine(p(12.0f, 11.2f), p(12.0f, 17.2f), brush, stroke * 0.9f, roundStroke.Get());
                rt->DrawLine(p(9.0f, 14.2f), p(15.0f, 14.2f), brush, stroke * 0.9f, roundStroke.Get());
            }
            break;
        }

        // ── 32. 锁定选区比例 (Aspect Ratio / Proportions) ───────────────────────────
        case CaptureIconId::PropAspectRatio: {
            auto frameBox = D2D1::RectF(p(3.0f, 5.5f).x, p(3.0f, 5.5f).y, p(21.0f, 18.5f).x, p(21.0f, 18.5f).y);
            rt->DrawRoundedRectangle(D2D1::RoundedRect(frameBox, 2.2f * s, 2.2f * s), brush, stroke, roundStroke.Get());
            drawPolyline({ p(6.5f, 11.5f), p(6.5f, 8.5f), p(9.5f, 8.5f) }, false, stroke * 0.9f);
            drawPolyline({ p(17.5f, 12.5f), p(17.5f, 15.5f), p(14.5f, 15.5f) }, false, stroke * 0.9f);
            break;
        }

        // ── 33. 方角序号微晶胶囊 (Rounded Square Number Badge) ─────────────────────
        case CaptureIconId::PropNumberSquare: {
            auto sqBox = D2D1::RectF(p(3.8f, 3.8f).x, p(3.8f, 3.8f).y, p(20.2f, 20.2f).x, p(20.2f, 20.2f).y);
            rt->DrawRoundedRectangle(D2D1::RoundedRect(sqBox, 4.0f * s, 4.0f * s), brush, stroke, roundStroke.Get());
            drawPolyline({ p(9.8f, 9.2f), p(12.2f, 7.4f), p(12.2f, 16.4f) }, false, stroke);
            rt->DrawLine(p(9.6f, 16.4f), p(14.8f, 16.4f), brush, stroke, roundStroke.Get());
            break;
        }

        // ── 34. 文字描边 (清晰大号 A 字 + 底部描边指示横条，彻底告别双竖线 Π 歧义) ──
        case CaptureIconId::PropTextOutline: {
            drawPolyline({ p(6.2f, 16.2f), p(12.0f, 4.2f), p(17.8f, 16.2f) }, false, stroke * 1.05f);
            rt->DrawLine(p(8.2f, 12.4f), p(15.8f, 12.4f), brush, stroke * 0.95f, roundStroke.Get());
            auto bar = D2D1::RectF(p(4.6f, 18.8f).x, p(4.6f, 18.8f).y, p(19.4f, 20.6f).x, p(19.4f, 20.6f).y);
            rt->FillRoundedRectangle(D2D1::RoundedRect(bar, 0.9f * s, 0.9f * s), brush);
            break;
        }

        // ── 35. 字号大小 (Typography Size: 大小双 T 对比标识) ───────────────────────
        case CaptureIconId::PropFontSize: {
            // 左侧大 T
            rt->DrawLine(p(3.0f, 5.8f), p(13.2f, 5.8f), brush, stroke, roundStroke.Get());
            rt->DrawLine(p(8.1f, 5.8f), p(8.1f, 18.6f), brush, stroke, roundStroke.Get());
            // 右侧小 T
            rt->DrawLine(p(13.8f, 11.2f), p(21.0f, 11.2f), brush, stroke * 0.9f, roundStroke.Get());
            rt->DrawLine(p(17.4f, 11.2f), p(17.4f, 18.6f), brush, stroke * 0.9f, roundStroke.Get());
            break;
        }

        // ── 36. 文字背景胶囊 (半透明圆角胶囊底板 + 居中清晰 A 字) ───────────────────
        case CaptureIconId::PropTextBackground: {
            auto pill = D2D1::RectF(p(3.0f, 4.4f).x, p(3.0f, 4.4f).y, p(21.0f, 19.6f).x, p(21.0f, 19.6f).y);
            if (auto* solid = static_cast<ID2D1SolidColorBrush*>(brush)) {
                D2D1_COLOR_F c = solid->GetColor();
                c.a *= 0.22f;
                Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> fillBrush;
                if (SUCCEEDED(rt->CreateSolidColorBrush(c, fillBrush.GetAddressOf())) && fillBrush) {
                    rt->FillRoundedRectangle(D2D1::RoundedRect(pill, 4.0f * s, 4.0f * s), fillBrush.Get());
                }
            }
            rt->DrawRoundedRectangle(D2D1::RoundedRect(pill, 4.0f * s, 4.0f * s), brush, stroke * 0.92f, roundStroke.Get());
            drawPolyline({ p(8.0f, 16.0f), p(12.0f, 7.6f), p(16.0f, 16.0f) }, false, stroke * 0.95f);
            rt->DrawLine(p(9.3f, 13.4f), p(14.7f, 13.4f), brush, stroke * 0.88f, roundStroke.Get());
            break;
        }

        // ── 37. 反选区域 (Invert Selection: 内实心框 + 外层四角选区) ─────────────────
        case CaptureIconId::ActionInvertSelection: {
            drawPolyline({ p(3.8f, 8.0f), p(3.8f, 3.8f), p(8.0f, 3.8f) }, false, stroke);
            drawPolyline({ p(16.0f, 3.8f), p(20.2f, 3.8f), p(20.2f, 8.0f) }, false, stroke);
            drawPolyline({ p(3.8f, 16.0f), p(3.8f, 20.2f), p(8.0f, 20.2f) }, false, stroke);
            drawPolyline({ p(16.0f, 20.2f), p(20.2f, 20.2f), p(20.2f, 16.0f) }, false, stroke);
            auto inner = D2D1::RectF(p(8.2f, 8.2f).x, p(8.2f, 8.2f).y, p(15.8f, 15.8f).x, p(15.8f, 15.8f).y);
            rt->FillRoundedRectangle(D2D1::RoundedRect(inner, 1.5f * s, 1.5f * s), brush);
            break;
        }

        default:
            break;
    }
}

} // namespace tools3000::capture
