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

    // 基于标准 Lucide 24x24 视口网格系统 (原点在 12, 12)，等比缩放至当前按钮中心
    const float s = std::min(btnW, btnH) / 24.0f * 0.68f;
    const float stroke = std::max(1.8f, 2.0f * scale);

    auto p = [&](float x, float y) -> D2D1_POINT_2F {
        return D2D1::Point2F(cx + (x - 12.0f) * s, cy + (y - 12.0f) * s);
    };

    // 创建高质量圆角/圆头描边样式 (Round Join & Round Cap)，彻底抹除直角折点毛刺
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

    switch (iconId) {
        // ── 1. 矩形工具 (Lucide: Square / rect x=3, y=3, w=18, h=18, rx=2) ───────────
        case CaptureIconId::ToolRectangle: {
            auto box = D2D1::RectF(cx - 8.5f * s, cy - 8.5f * s, cx + 8.5f * s, cy + 8.5f * s);
            rt->DrawRoundedRectangle(D2D1::RoundedRect(box, 2.5f * s, 2.5f * s), brush, stroke, roundStroke.Get());
            break;
        }

        // ── 1.1 直线工具 (Reicon / Lucide: Slash / 45° 直线) ─────────────
        case CaptureIconId::ToolLine: {
            rt->DrawLine(p(4, 20), p(20, 4), brush, stroke * 1.35f, roundStroke.Get());
            break;
        }

        // ── 2. 椭圆工具 (Lucide: Circle / cx=12, cy=12, r=10) ────────────────────────
        case CaptureIconId::ToolEllipse: {
            auto ellipse = D2D1::Ellipse(D2D1::Point2F(cx, cy), 9.0f * s, 9.0f * s);
            rt->DrawEllipse(ellipse, brush, stroke, roundStroke.Get());
            break;
        }

        // ── 3. 45° 标注箭头 (Lucide: ArrowUpRight / M7 7h10v10 + M7 17 17 7 顶级矢量设计) ──
        case CaptureIconId::ToolArrow: {
            const float arrowStroke = std::max(1.8f, 2.0f * scale);
            // 45° 斜向箭身
            rt->DrawLine(p(7, 17), p(17, 7), brush, arrowStroke, roundStroke.Get());
            // 优雅头部折角 (Round Join / Round Cap 黄金比例开角)
            auto chevron = createPath({ p(8, 7), p(17, 7), p(17, 16) }, false);
            if (chevron) {
                rt->DrawGeometry(chevron.Get(), brush, arrowStroke, roundStroke.Get());
            } else {
                rt->DrawLine(p(8, 7), p(17, 7), brush, arrowStroke, roundStroke.Get());
                rt->DrawLine(p(17, 7), p(17, 16), brush, arrowStroke, roundStroke.Get());
            }
            break;
        }

        // ── 3.1 水平标准实心几何单向箭头 (Classic Solid Filled Triangle) ───────────
        case CaptureIconId::ToolArrowStandard: {
            const float arrowStroke = std::max(1.8f, 2.0f * scale);
            // 水平箭身连接至几何三角底边
            rt->DrawLine(p(4, 12), p(14, 12), brush, arrowStroke, roundStroke.Get());
            // 经典实心尖角头部
            auto head = createPath({ p(20.5f, 12), p(13.5f, 7.5f), p(13.5f, 16.5f) }, true);
            if (head) {
                rt->FillGeometry(head.Get(), brush);
                rt->DrawGeometry(head.Get(), brush, 1.0f * scale, roundStroke.Get());
            }
            break;
        }

        // ── 4. 水平细线开放箭头 (Lucide: ArrowRight / M5 12h14 + m12 5 7 7-7 7) ───────
        case CaptureIconId::ToolArrowThin: {
            const float arrowStroke = std::max(1.8f, 2.0f * scale);
            // 水平贯穿箭身
            rt->DrawLine(p(4, 12), p(19, 12), brush, arrowStroke, roundStroke.Get());
            // 极简细线开放折角
            auto chevron = createPath({ p(13.5f, 6.5f), p(19, 12), p(13.5f, 17.5f) }, false);
            if (chevron) {
                rt->DrawGeometry(chevron.Get(), brush, arrowStroke, roundStroke.Get());
            } else {
                rt->DrawLine(p(13.5f, 6.5f), p(19, 12), brush, arrowStroke, roundStroke.Get());
                rt->DrawLine(p(19, 12), p(13.5f, 17.5f), brush, arrowStroke, roundStroke.Get());
            }
            break;
        }

        // ── 4.1 水平圆润科技机翼箭头 (Modern Swept Aerodynamic Wing) ────────────────
        case CaptureIconId::ToolArrowWing: {
            const float arrowStroke = std::max(1.8f, 2.0f * scale);
            // 箭身连接至内凹凹槽
            rt->DrawLine(p(4, 12), p(15.5f, 12), brush, arrowStroke, roundStroke.Get());
            // 科技流线机翼型四边形箭头 (Tip -> UpperWing -> Notch -> LowerWing)
            auto wing = createPath({ p(20.5f, 12), p(13.0f, 6.5f), p(15.5f, 12), p(13.0f, 17.5f) }, true);
            if (wing) {
                rt->FillGeometry(wing.Get(), brush);
                rt->DrawGeometry(wing.Get(), brush, 1.0f * scale, roundStroke.Get());
            }
            break;
        }

        // ── 5. 水平双向箭头 (Lucide: MoveHorizontal / 双向对称折角) ─────────────────
        case CaptureIconId::ToolArrowDouble: {
            const float arrowStroke = std::max(1.8f, 2.0f * scale);
            // 贯穿水平箭身
            rt->DrawLine(p(5, 12), p(19, 12), brush, arrowStroke, roundStroke.Get());
            // 左侧开放折角
            auto leftChevron = createPath({ p(9.5f, 7.5f), p(4.5f, 12), p(9.5f, 16.5f) }, false);
            if (leftChevron) {
                rt->DrawGeometry(leftChevron.Get(), brush, arrowStroke, roundStroke.Get());
            } else {
                rt->DrawLine(p(9.5f, 7.5f), p(4.5f, 12), brush, arrowStroke, roundStroke.Get());
                rt->DrawLine(p(4.5f, 12), p(9.5f, 16.5f), brush, arrowStroke, roundStroke.Get());
            }
            // 右侧开放折角
            auto rightChevron = createPath({ p(14.5f, 7.5f), p(19.5f, 12), p(14.5f, 16.5f) }, false);
            if (rightChevron) {
                rt->DrawGeometry(rightChevron.Get(), brush, arrowStroke, roundStroke.Get());
            } else {
                rt->DrawLine(p(14.5f, 7.5f), p(19.5f, 12), brush, arrowStroke, roundStroke.Get());
                rt->DrawLine(p(19.5f, 12), p(14.5f, 16.5f), brush, arrowStroke, roundStroke.Get());
            }
            break;
        }

        // ── 6. 铅笔 / 画笔 (Lucide: Pencil / M17 3... + tip) ──────────────────────────
        case CaptureIconId::ToolPen: {
            rt->DrawLine(p(17, 3), p(21, 7), brush, stroke, roundStroke.Get());
            rt->DrawLine(p(21, 7), p(7.5f, 20.5f), brush, stroke, roundStroke.Get());
            rt->DrawLine(p(7.5f, 20.5f), p(2, 22), brush, stroke, roundStroke.Get());
            rt->DrawLine(p(2, 22), p(3.5f, 16.5f), brush, stroke, roundStroke.Get());
            rt->DrawLine(p(3.5f, 16.5f), p(17, 3), brush, stroke, roundStroke.Get());
            rt->DrawLine(p(14, 6), p(18, 10), brush, stroke * 0.85f, roundStroke.Get());
            break;
        }

        // ── 7. 荧光笔 (Lucide: Highlighter) ─────────────────────────────────────────
        case CaptureIconId::ToolHighlight: {
            rt->DrawLine(p(9, 11), p(3, 17), brush, stroke, roundStroke.Get());
            rt->DrawLine(p(3, 17), p(3, 20), brush, stroke, roundStroke.Get());
            rt->DrawLine(p(3, 20), p(12, 20), brush, stroke, roundStroke.Get());
            rt->DrawLine(p(12, 20), p(15, 17), brush, stroke, roundStroke.Get());
            rt->DrawLine(p(9, 11), p(15, 17), brush, stroke, roundStroke.Get());
            rt->DrawLine(p(14, 4), p(20, 10), brush, stroke, roundStroke.Get());
            rt->DrawLine(p(14, 4), p(11.4f, 6.6f), brush, stroke, roundStroke.Get());
            rt->DrawLine(p(20, 10), p(17.4f, 12.6f), brush, stroke, roundStroke.Get());
            break;
        }

        // ── 8. 马赛克 (Lucide: Grid2x2 / 2x2 网格) ──────────────────────────────────
        case CaptureIconId::ToolMosaic: {
            auto box = D2D1::RectF(cx - 8.5f * s, cy - 8.5f * s, cx + 8.5f * s, cy + 8.5f * s);
            rt->DrawRoundedRectangle(D2D1::RoundedRect(box, 2.5f * s, 2.5f * s), brush, stroke, roundStroke.Get());
            rt->DrawLine(p(12, 3.5f), p(12, 20.5f), brush, stroke, roundStroke.Get());
            rt->DrawLine(p(3.5f, 12), p(20.5f, 12), brush, stroke, roundStroke.Get());
            // 左上与右下填充精美半透实心微格
            auto tl = D2D1::RectF(cx - 8.0f * s, cy - 8.0f * s, cx - 0.5f * s, cy - 0.5f * s);
            auto brBox = D2D1::RectF(cx + 0.5f * s, cy + 0.5f * s, cx + 8.0f * s, cy + 8.0f * s);
            rt->FillRectangle(tl, brush);
            rt->FillRectangle(brBox, brush);
            break;
        }

        // ── 8.5. 模糊滤镜 (Lucide: Droplet / 柔和水滴) ──────────────────────────────
        case CaptureIconId::ToolBlur: {
            rt->DrawLine(p(12, 3), p(6.5f, 13), brush, stroke, roundStroke.Get());
            rt->DrawLine(p(12, 3), p(17.5f, 13), brush, stroke, roundStroke.Get());
            rt->DrawLine(p(6.5f, 13), p(8, 18), brush, stroke, roundStroke.Get());
            rt->DrawLine(p(17.5f, 13), p(16, 18), brush, stroke, roundStroke.Get());
            rt->DrawLine(p(8, 18), p(12, 21), brush, stroke, roundStroke.Get());
            rt->DrawLine(p(16, 18), p(12, 21), brush, stroke, roundStroke.Get());
            break;
        }

        // ── 9. 文字 T (Lucide: Type / M12 4v16 + M4 7V5... + M9 20h6) ─────────────────
        case CaptureIconId::ToolText: {
            rt->DrawLine(p(12, 4), p(12, 20), brush, stroke, roundStroke.Get());
            rt->DrawLine(p(4, 7), p(4, 5), brush, stroke, roundStroke.Get());
            rt->DrawLine(p(4, 5), p(20, 5), brush, stroke, roundStroke.Get());
            rt->DrawLine(p(20, 5), p(20, 7), brush, stroke, roundStroke.Get());
            rt->DrawLine(p(9, 20), p(15, 20), brush, stroke, roundStroke.Get());
            break;
        }

        // ── 10. 序号气泡 ① (Circle + Clean 1) ───────────────────────────────────────
        case CaptureIconId::ToolNumber: {
            auto ellipse = D2D1::Ellipse(D2D1::Point2F(cx, cy), 9.0f * s, 9.0f * s);
            rt->DrawEllipse(ellipse, brush, stroke, roundStroke.Get());
            rt->DrawLine(p(10, 9), p(12, 7.5f), brush, stroke, roundStroke.Get());
            rt->DrawLine(p(12, 7.5f), p(12, 16.5f), brush, stroke, roundStroke.Get());
            rt->DrawLine(p(9.5f, 16.5f), p(14.5f, 16.5f), brush, stroke, roundStroke.Get());
            break;
        }

        // ── 11. 智能消除 (Lucide: Wand2 / 魔法棒 + 4 芒星) ──────────────────────────
        case CaptureIconId::ToolInpaint: {
            // 魔棒棒身
            rt->DrawLine(p(3.5f, 20.5f), p(18.5f, 5.5f), brush, stroke * 1.6f, roundStroke.Get());
            rt->DrawLine(p(13.5f, 8.5f), p(15.5f, 10.5f), brush, stroke * 0.8f, roundStroke.Get());
            // 右上 4 芒星光斑
            rt->DrawLine(p(19, 2), p(19, 6), brush, stroke * 0.9f, roundStroke.Get());
            rt->DrawLine(p(17, 4), p(21, 4), brush, stroke * 0.9f, roundStroke.Get());
            // 左上小星光
            rt->DrawLine(p(7, 3), p(7, 5), brush, stroke * 0.75f, roundStroke.Get());
            rt->DrawLine(p(6, 4), p(8, 4), brush, stroke * 0.75f, roundStroke.Get());
            break;
        }

        // ── 12. 撤销 (Lucide: Undo2 / M9 14 4 9l5-5 + M4 9h10.5a5.5...) ───────────────
        case CaptureIconId::ActionUndo: {
            rt->DrawLine(p(9, 14), p(4, 9), brush, stroke, roundStroke.Get());
            rt->DrawLine(p(4, 9), p(9, 4), brush, stroke, roundStroke.Get());
            rt->DrawLine(p(4, 9), p(14.5f, 9), brush, stroke, roundStroke.Get());
            // 右侧顺滑圆弧回折 (从 (14.5, 9) 弯至 (14.5, 20) 到 (11, 20))
            rt->DrawLine(p(14.5f, 9), p(18.5f, 11), brush, stroke, roundStroke.Get());
            rt->DrawLine(p(18.5f, 11), p(19.5f, 14.5f), brush, stroke, roundStroke.Get());
            rt->DrawLine(p(19.5f, 14.5f), p(18.5f, 18), brush, stroke, roundStroke.Get());
            rt->DrawLine(p(18.5f, 18), p(14.5f, 20), brush, stroke, roundStroke.Get());
            rt->DrawLine(p(14.5f, 20), p(10, 20), brush, stroke, roundStroke.Get());
            break;
        }

        // ── 13. 重做 (Lucide: Redo2 / m15 14 5-5-5-5 + M20 9H9.5a5.5...) ──────────────
        case CaptureIconId::ActionRedo: {
            rt->DrawLine(p(15, 14), p(20, 9), brush, stroke, roundStroke.Get());
            rt->DrawLine(p(20, 9), p(15, 4), brush, stroke, roundStroke.Get());
            rt->DrawLine(p(20, 9), p(9.5f, 9), brush, stroke, roundStroke.Get());
            // 左侧顺滑圆弧回折
            rt->DrawLine(p(9.5f, 9), p(5.5f, 11), brush, stroke, roundStroke.Get());
            rt->DrawLine(p(5.5f, 11), p(4.5f, 14.5f), brush, stroke, roundStroke.Get());
            rt->DrawLine(p(4.5f, 14.5f), p(5.5f, 18), brush, stroke, roundStroke.Get());
            rt->DrawLine(p(5.5f, 18), p(9.5f, 20), brush, stroke, roundStroke.Get());
            rt->DrawLine(p(9.5f, 20), p(14, 20), brush, stroke, roundStroke.Get());
            break;
        }

        // ── 14. 清空 / 垃圾桶 (Lucide: Trash2) ───────────────────────────────────────
        case CaptureIconId::ActionClear: {
            rt->DrawLine(p(3, 6), p(21, 6), brush, stroke, roundStroke.Get());
            auto lid = createPath({ p(8, 6), p(8, 4), p(16, 4), p(16, 6) }, false);
            if (lid) {
                rt->DrawGeometry(lid.Get(), brush, stroke, roundStroke.Get());
            } else {
                rt->DrawLine(p(8, 6), p(8, 4), brush, stroke, roundStroke.Get());
                rt->DrawLine(p(8, 4), p(16, 4), brush, stroke, roundStroke.Get());
                rt->DrawLine(p(16, 4), p(16, 6), brush, stroke, roundStroke.Get());
            }
            auto bin = D2D1::RectF(cx - 6.5f * s, cy - 6.0f * s, cx + 6.5f * s, cy + 9.0f * s);
            rt->DrawRoundedRectangle(D2D1::RoundedRect(bin, 2.0f * s, 2.0f * s), brush, stroke, roundStroke.Get());
            rt->DrawLine(p(10, 10), p(10, 16), brush, stroke, roundStroke.Get());
            rt->DrawLine(p(14, 10), p(14, 16), brush, stroke, roundStroke.Get());
            break;
        }

        // ── 15. OCR 文本提取 (Lucide: ScanText / 四角对焦 + 3 道扫描文本线) ────────────
        case CaptureIconId::ActionExtractText: {
            // 四角对焦
            auto c1 = createPath({ p(3, 8), p(3, 5), p(6, 5) }, false);
            auto c2 = createPath({ p(18, 5), p(21, 5), p(21, 8) }, false);
            auto c3 = createPath({ p(3, 16), p(3, 19), p(6, 19) }, false);
            auto c4 = createPath({ p(18, 19), p(21, 19), p(21, 16) }, false);
            if (c1 && c2 && c3 && c4) {
                rt->DrawGeometry(c1.Get(), brush, stroke, roundStroke.Get());
                rt->DrawGeometry(c2.Get(), brush, stroke, roundStroke.Get());
                rt->DrawGeometry(c3.Get(), brush, stroke, roundStroke.Get());
                rt->DrawGeometry(c4.Get(), brush, stroke, roundStroke.Get());
            } else {
                rt->DrawLine(p(3, 8), p(3, 5), brush, stroke, roundStroke.Get());
                rt->DrawLine(p(3, 5), p(6, 5), brush, stroke, roundStroke.Get());
                rt->DrawLine(p(18, 5), p(21, 5), brush, stroke, roundStroke.Get());
                rt->DrawLine(p(21, 5), p(21, 8), brush, stroke, roundStroke.Get());
                rt->DrawLine(p(3, 16), p(3, 19), brush, stroke, roundStroke.Get());
                rt->DrawLine(p(3, 19), p(6, 19), brush, stroke, roundStroke.Get());
                rt->DrawLine(p(18, 19), p(21, 19), brush, stroke, roundStroke.Get());
                rt->DrawLine(p(21, 19), p(21, 16), brush, stroke, roundStroke.Get());
            }

            // 内部 3 条优雅排版横线
            rt->DrawLine(p(7, 8), p(15, 8), brush, stroke * 0.9f, roundStroke.Get());
            rt->DrawLine(p(7, 12), p(17, 12), brush, stroke * 0.9f, roundStroke.Get());
            rt->DrawLine(p(7, 16), p(13, 16), brush, stroke * 0.9f, roundStroke.Get());
            break;
        }

        // ── 16. 图钉 (Lucide: Pin) ──────────────────────────────────────────────────
        case CaptureIconId::ActionPinWindow: {
            rt->DrawLine(p(12, 16), p(12, 22), brush, stroke, roundStroke.Get());
            rt->DrawLine(p(5, 15), p(19, 15), brush, stroke, roundStroke.Get());
            rt->DrawLine(p(7, 15), p(8.5f, 11), brush, stroke, roundStroke.Get());
            rt->DrawLine(p(17, 15), p(15.5f, 11), brush, stroke, roundStroke.Get());
            rt->DrawLine(p(8.5f, 11), p(8.5f, 6), brush, stroke, roundStroke.Get());
            rt->DrawLine(p(15.5f, 11), p(15.5f, 6), brush, stroke, roundStroke.Get());
            rt->DrawLine(p(7, 6), p(17, 6), brush, stroke, roundStroke.Get());
            rt->DrawLine(p(9, 6), p(9, 3), brush, stroke, roundStroke.Get());
            rt->DrawLine(p(15, 6), p(15, 3), brush, stroke, roundStroke.Get());
            rt->DrawLine(p(9, 3), p(15, 3), brush, stroke, roundStroke.Get());
            break;
        }

        // ── 17. 长截图 (Lucide: ArrowDownToLine / 窗口 + 贯穿向下长箭头) ──────────────
        case CaptureIconId::ActionScrollCapture: {
            rt->DrawLine(p(12, 3), p(12, 17), brush, stroke, roundStroke.Get());
            auto head = createPath({ p(6.5f, 11.5f), p(12, 17), p(17.5f, 11.5f) }, false);
            if (head) {
                rt->DrawGeometry(head.Get(), brush, stroke, roundStroke.Get());
            } else {
                rt->DrawLine(p(6.5f, 11.5f), p(12, 17), brush, stroke, roundStroke.Get());
                rt->DrawLine(p(17.5f, 11.5f), p(12, 17), brush, stroke, roundStroke.Get());
            }
            rt->DrawLine(p(5, 21), p(19, 21), brush, stroke, roundStroke.Get());
            break;
        }

        // ── 18. 取消 (Lucide: X / M18 6 6 18 + m6 6 12 12) ───────────────────────────
        case CaptureIconId::ActionCancel: {
            rt->DrawLine(p(18, 6), p(6, 18), brush, stroke, roundStroke.Get());
            rt->DrawLine(p(6, 6), p(18, 18), brush, stroke, roundStroke.Get());
            break;
        }

        // ── 19. 确认 (Lucide: Check / M20 6 9 17l-5-5) ──────────────────────────────
        case CaptureIconId::ActionConfirm: {
            auto check = createPath({ p(4.5f, 12.0f), p(9.5f, 17.0f), p(19.5f, 7.0f) }, false);
            if (check) {
                rt->DrawGeometry(check.Get(), brush, stroke * 1.35f, roundStroke.Get());
            } else {
                rt->DrawLine(p(4.5f, 12.0f), p(9.5f, 17.0f), brush, stroke * 1.35f, roundStroke.Get());
                rt->DrawLine(p(9.5f, 17.0f), p(19.5f, 7.0f), brush, stroke * 1.35f, roundStroke.Get());
            }
            break;
        }

        // ── 19.0 录屏摄影机 (ReIcon / Lucide: Video / 经典 2px 矢量摄像机) ─────────
        case CaptureIconId::ActionRecordVideo: {
            // 摄像机机身 (rounded rect x=2.5, y=6.5, w=12.5, h=11, rx=2.5)
            auto body = D2D1::RectF(p(2.5f, 6.5f).x, p(2.5f, 6.5f).y, p(15.0f, 17.5f).x, p(15.0f, 17.5f).y);
            rt->DrawRoundedRectangle(D2D1::RoundedRect(body, 2.5f * s, 2.5f * s), brush, stroke, roundStroke.Get());
            // 摄像机出光镜头 (梯形投影镜头: (15, 10.5) -> (21.5, 7.5) -> (21.5, 16.5) -> (15, 13.5))
            auto lensPath = createPath({p(15.0f, 10.5f), p(21.5f, 7.5f), p(21.5f, 16.5f), p(15.0f, 13.5f)}, true);
            if (lensPath) {
                rt->DrawGeometry(lensPath.Get(), brush, stroke, roundStroke.Get());
            }
            break;
        }

        // ── 19.1 录屏开始 (实心正红高亮圆) ──────────────────────────────────────────
        case CaptureIconId::ActionRecordStart: {
            auto circle = D2D1::Ellipse(D2D1::Point2F(cx, cy), 6.5f * s, 6.5f * s);
            rt->FillEllipse(circle, brush);
            break;
        }

        // ── 19.2 录屏暂停 (经典双竖线) ──────────────────────────────────────────────
        case CaptureIconId::ActionRecordPause: {
            rt->DrawLine(p(9, 6), p(9, 18), brush, stroke * 1.6f);
            rt->DrawLine(p(15, 6), p(15, 18), brush, stroke * 1.6f);
            break;
        }

        // ── 19.3 录屏停止 (实心停止方块) ────────────────────────────────────────────
        case CaptureIconId::ActionRecordStop: {
            auto box = D2D1::RectF(cx - 5.5f * s, cy - 5.5f * s, cx + 5.5f * s, cy + 5.5f * s);
            rt->FillRoundedRectangle(D2D1::RoundedRect(box, 2.0f * s, 2.0f * s), brush);
            break;
        }

        // ── 19.4 麦克风 (Lucide: Mic) ───────────────────────────────────────────────
        case CaptureIconId::ActionToggleMic: {
            auto micCap = D2D1::RectF(cx - 3.5f * s, cy - 8.0f * s, cx + 3.5f * s, cy + 2.0f * s);
            rt->DrawRoundedRectangle(D2D1::RoundedRect(micCap, 3.5f * s, 3.5f * s), brush, stroke, roundStroke.Get());
            // U 型托架
            auto cradle = createPath({ p(6, 10), p(6, 12), p(12, 17), p(18, 12), p(18, 10) }, false);
            if (cradle) {
                rt->DrawGeometry(cradle.Get(), brush, stroke, roundStroke.Get());
            } else {
                rt->DrawLine(p(6, 10), p(6, 12), brush, stroke, roundStroke.Get());
                rt->DrawLine(p(6, 12), p(12, 17), brush, stroke, roundStroke.Get());
                rt->DrawLine(p(18, 12), p(12, 17), brush, stroke, roundStroke.Get());
                rt->DrawLine(p(18, 10), p(18, 12), brush, stroke, roundStroke.Get());
            }
            // 立柱与底座
            rt->DrawLine(p(12, 17), p(12, 21), brush, stroke, roundStroke.Get());
            rt->DrawLine(p(8, 21), p(16, 21), brush, stroke, roundStroke.Get());
            break;
        }

        // ── 19.5 扬声器 (Lucide: Volume2) ───────────────────────────────────────────
        case CaptureIconId::ActionToggleSpeaker: {
            auto speaker = createPath({ p(4, 9), p(8, 9), p(13, 5), p(13, 19), p(8, 15), p(4, 15) }, true);
            if (speaker) {
                rt->DrawGeometry(speaker.Get(), brush, stroke, roundStroke.Get());
            } else {
                rt->DrawLine(p(4, 9), p(8, 9), brush, stroke, roundStroke.Get());
                rt->DrawLine(p(8, 9), p(13, 5), brush, stroke, roundStroke.Get());
                rt->DrawLine(p(13, 5), p(13, 19), brush, stroke, roundStroke.Get());
                rt->DrawLine(p(13, 19), p(8, 15), brush, stroke, roundStroke.Get());
                rt->DrawLine(p(8, 15), p(4, 15), brush, stroke, roundStroke.Get());
                rt->DrawLine(p(4, 15), p(4, 9), brush, stroke, roundStroke.Get());
            }
            // 声波弧线
            rt->DrawLine(p(16, 9), p(18, 12), brush, stroke, roundStroke.Get());
            rt->DrawLine(p(18, 12), p(16, 15), brush, stroke, roundStroke.Get());
            rt->DrawLine(p(19, 6), p(22, 12), brush, stroke, roundStroke.Get());
            rt->DrawLine(p(22, 12), p(19, 18), brush, stroke, roundStroke.Get());
            break;
        }

        // ── 19.6 复制 (Lucide: Copy) ────────────────────────────────────────────────
        case CaptureIconId::ActionCopy: {
            // 后层矩形 (右上折线)
            auto backBox = createPath({ p(8, 4), p(19, 4), p(19, 15) }, false);
            if (backBox) {
                rt->DrawGeometry(backBox.Get(), brush, stroke, roundStroke.Get());
            } else {
                rt->DrawLine(p(8, 4), p(19, 4), brush, stroke, roundStroke.Get());
                rt->DrawLine(p(19, 4), p(19, 15), brush, stroke, roundStroke.Get());
            }
            // 前层圆角矩形
            auto frontBox = D2D1::RectF(p(4, 8).x, p(4, 8).y, p(15, 19).x, p(15, 19).y);
            rt->DrawRoundedRectangle(D2D1::RoundedRect(frontBox, 2.0f * s, 2.0f * s), brush, stroke, roundStroke.Get());
            break;
        }

        // ── 19.7 保存 / 下载 (Lucide: Download) ─────────────────────────────────────
        case CaptureIconId::ActionSave: {
            // 下箭头
            rt->DrawLine(p(12, 4), p(12, 14.5f), brush, stroke, roundStroke.Get());
            auto arrowHead = createPath({ p(7.5f, 10.0f), p(12.0f, 14.5f), p(16.5f, 10.0f) }, false);
            if (arrowHead) {
                rt->DrawGeometry(arrowHead.Get(), brush, stroke, roundStroke.Get());
            } else {
                rt->DrawLine(p(7.5f, 10.0f), p(12.0f, 14.5f), brush, stroke, roundStroke.Get());
                rt->DrawLine(p(16.5f, 10.0f), p(12.0f, 14.5f), brush, stroke, roundStroke.Get());
            }
            // 底部圆角托盘
            auto tray = createPath({ p(4.5f, 16.5f), p(4.5f, 20.0f), p(19.5f, 20.0f), p(19.5f, 16.5f) }, false);
            if (tray) {
                rt->DrawGeometry(tray.Get(), brush, stroke, roundStroke.Get());
            } else {
                rt->DrawLine(p(4.5f, 16.5f), p(4.5f, 20.0f), brush, stroke, roundStroke.Get());
                rt->DrawLine(p(4.5f, 20.0f), p(19.5f, 20.0f), brush, stroke, roundStroke.Get());
                rt->DrawLine(p(19.5f, 20.0f), p(19.5f, 16.5f), brush, stroke, roundStroke.Get());
            }
            break;
        }

        // ── 19.8 反色 / 负片 (Reicon: Contrast / 半实心圆) ─────────────────────────
        case CaptureIconId::ActionInvert: {
            auto circle = D2D1::Ellipse(D2D1::Point2F(cx, cy), 9.0f * s, 9.0f * s);
            rt->DrawEllipse(circle, brush, stroke, roundStroke.Get());
            for (float dy = -7.0f; dy <= 7.0f; dy += 2.2f) {
                float halfW = std::sqrt(std::max(0.0f, 9.0f * 9.0f - dy * dy)) * s;
                rt->DrawLine(D2D1::Point2F(cx, cy + dy * s), D2D1::Point2F(cx + halfW, cy + dy * s), brush, stroke * 0.9f, roundStroke.Get());
            }
            break;
        }

        // ── 19.9 灰度黑白 (Reicon: Layers / 双重单色阶) ─────────────────────────────
        case CaptureIconId::ActionGrayscale: {
            auto box = D2D1::RectF(cx - 8.5f * s, cy - 8.5f * s, cx + 8.5f * s, cy + 8.5f * s);
            rt->DrawRoundedRectangle(D2D1::RoundedRect(box, 2.5f * s, 2.5f * s), brush, stroke, roundStroke.Get());
            rt->DrawLine(D2D1::Point2F(cx, cy - 8.5f * s), D2D1::Point2F(cx, cy + 8.5f * s), brush, stroke, roundStroke.Get());
            rt->DrawLine(p(12, 6), p(18, 12), brush, stroke * 0.8f, roundStroke.Get());
            rt->DrawLine(p(12, 12), p(18, 18), brush, stroke * 0.8f, roundStroke.Get());
            break;
        }

        // ── 19.10 1:1 原始尺寸 (Reicon: Scale / 四角定焦标) ─────────────────────────
        case CaptureIconId::ActionResetScale: {
            auto c1 = createPath({ p(4, 8), p(4, 4), p(8, 4) }, false);
            auto c2 = createPath({ p(16, 4), p(20, 4), p(20, 8) }, false);
            auto c3 = createPath({ p(4, 16), p(4, 20), p(8, 20) }, false);
            auto c4 = createPath({ p(16, 20), p(20, 20), p(20, 16) }, false);
            if (c1 && c2 && c3 && c4) {
                rt->DrawGeometry(c1.Get(), brush, stroke, roundStroke.Get());
                rt->DrawGeometry(c2.Get(), brush, stroke, roundStroke.Get());
                rt->DrawGeometry(c3.Get(), brush, stroke, roundStroke.Get());
                rt->DrawGeometry(c4.Get(), brush, stroke, roundStroke.Get());
            } else {
                rt->DrawLine(p(4, 8), p(4, 4), brush, stroke, roundStroke.Get());
                rt->DrawLine(p(4, 4), p(8, 4), brush, stroke, roundStroke.Get());
                rt->DrawLine(p(16, 4), p(20, 4), brush, stroke, roundStroke.Get());
                rt->DrawLine(p(20, 4), p(20, 8), brush, stroke, roundStroke.Get());
                rt->DrawLine(p(4, 16), p(4, 20), brush, stroke, roundStroke.Get());
                rt->DrawLine(p(4, 20), p(8, 20), brush, stroke, roundStroke.Get());
                rt->DrawLine(p(16, 20), p(20, 20), brush, stroke, roundStroke.Get());
                rt->DrawLine(p(20, 20), p(20, 16), brush, stroke, roundStroke.Get());
            }
            rt->DrawLine(p(11, 10), p(12, 8.5f), brush, stroke, roundStroke.Get());
            rt->DrawLine(p(12, 8.5f), p(12, 15.5f), brush, stroke, roundStroke.Get());
            rt->DrawLine(p(10, 15.5f), p(14, 15.5f), brush, stroke, roundStroke.Get());
            break;
        }

        // ── 19.11 折叠贴图 / 最小化微晶胶囊 (Reicon: Minimize2 / 折角双箭头) ────────
        case CaptureIconId::ActionFold: {
            auto arrow1 = createPath({ p(4, 14), p(10, 14), p(10, 20) }, false);
            if (arrow1) {
                rt->DrawGeometry(arrow1.Get(), brush, stroke, roundStroke.Get());
            } else {
                rt->DrawLine(p(4, 14), p(10, 14), brush, stroke, roundStroke.Get());
                rt->DrawLine(p(10, 14), p(10, 20), brush, stroke, roundStroke.Get());
            }
            rt->DrawLine(p(10, 14), p(3, 21), brush, stroke, roundStroke.Get());

            auto arrow2 = createPath({ p(20, 10), p(14, 10), p(14, 4) }, false);
            if (arrow2) {
                rt->DrawGeometry(arrow2.Get(), brush, stroke, roundStroke.Get());
            } else {
                rt->DrawLine(p(20, 10), p(14, 10), brush, stroke, roundStroke.Get());
                rt->DrawLine(p(14, 10), p(14, 4), brush, stroke, roundStroke.Get());
            }
            rt->DrawLine(p(14, 10), p(21, 3), brush, stroke, roundStroke.Get());
            break;
        }

        // ── 19.11b 旋转贴图 (Lucide: RotateCw / 顺时针环形箭头) ──────────────────────
        case CaptureIconId::ActionRotate: {
            auto arcPath = createPath({
                p(19.5f, 8.5f), p(16.8f, 5.2f), p(12.0f, 4.0f),
                p(6.8f, 5.5f),  p(4.2f, 9.8f),  p(4.5f, 15.0f),
                p(7.8f, 18.8f), p(12.8f, 20.0f), p(17.8f, 18.0f), p(19.8f, 14.2f)
            }, false);
            if (arcPath) {
                rt->DrawGeometry(arcPath.Get(), brush, stroke, roundStroke.Get());
            } else {
                rt->DrawLine(p(19.5f, 8.5f), p(12.0f, 4.0f), brush, stroke, roundStroke.Get());
                rt->DrawLine(p(12.0f, 4.0f), p(4.5f, 12.0f), brush, stroke, roundStroke.Get());
                rt->DrawLine(p(4.5f, 12.0f), p(12.0f, 20.0f), brush, stroke, roundStroke.Get());
                rt->DrawLine(p(12.0f, 20.0f), p(19.5f, 14.5f), brush, stroke, roundStroke.Get());
            }
            auto arrowHead = createPath({ p(20.0f, 3.8f), p(20.0f, 9.0f), p(14.8f, 9.0f) }, false);
            if (arrowHead) {
                rt->DrawGeometry(arrowHead.Get(), brush, stroke, roundStroke.Get());
            } else {
                rt->DrawLine(p(20.0f, 3.8f), p(20.0f, 9.0f), brush, stroke, roundStroke.Get());
                rt->DrawLine(p(20.0f, 9.0f), p(14.8f, 9.0f), brush, stroke, roundStroke.Get());
            }
            break;
        }

        // ── 19.12 重置序号 (Reicon: RefreshCw / 环形标 + 1) ────────────────────────
        case CaptureIconId::ActionResetNumber: {
            auto circle = D2D1::Ellipse(D2D1::Point2F(cx, cy), 8.5f * s, 8.5f * s);
            rt->DrawEllipse(circle, brush, stroke * 0.85f, roundStroke.Get());
            rt->DrawLine(p(11, 9), p(12, 7.5f), brush, stroke * 1.1f, roundStroke.Get());
            rt->DrawLine(p(12, 7.5f), p(12, 16.5f), brush, stroke * 1.1f, roundStroke.Get());
            rt->DrawLine(p(9.5f, 16.5f), p(14.5f, 16.5f), brush, stroke * 1.1f, roundStroke.Get());
            break;
        }

        // ── 19.13 录屏高清快照 (Reicon: Camera / 相机抓拍) ─────────────────────────
        case CaptureIconId::ActionSnapshot: {
            auto body = D2D1::RectF(cx - 8.5f * s, cy - 4.5f * s, cx + 8.5f * s, cy + 7.5f * s);
            rt->DrawRoundedRectangle(D2D1::RoundedRect(body, 2.2f * s, 2.2f * s), brush, stroke, roundStroke.Get());
            rt->DrawEllipse(D2D1::Ellipse(D2D1::Point2F(cx, cy + 1.5f * s), 3.8f * s, 3.8f * s), brush, stroke, roundStroke.Get());
            auto notch = createPath({ p(8.5f, 6.0f), p(10.0f, 3.5f), p(14.0f, 3.5f), p(15.5f, 6.0f) }, false);
            if (notch) {
                rt->DrawGeometry(notch.Get(), brush, stroke, roundStroke.Get());
            } else {
                rt->DrawLine(p(8.5f, 6.0f), p(10.0f, 3.5f), brush, stroke, roundStroke.Get());
                rt->DrawLine(p(10.0f, 3.5f), p(14.0f, 3.5f), brush, stroke, roundStroke.Get());
                rt->DrawLine(p(14.0f, 3.5f), p(15.5f, 6.0f), brush, stroke, roundStroke.Get());
            }
            rt->DrawEllipse(D2D1::Ellipse(p(16.0f, 9.0f), 0.8f * s, 0.8f * s), brush, stroke, roundStroke.Get());
            break;
        }

        // ── 19.14 打开所在文件夹 (Reicon: Folder / 文件夹) ─────────────────────────
        case CaptureIconId::ActionOpenFolder: {
            auto folderPath = createPath({ p(3.5f, 7.0f), p(8.5f, 7.0f), p(10.5f, 9.5f), p(20.5f, 9.5f), p(20.5f, 18.5f), p(3.5f, 18.5f) }, true);
            if (folderPath) {
                rt->DrawGeometry(folderPath.Get(), brush, stroke, roundStroke.Get());
            } else {
                rt->DrawLine(p(3.5f, 7.0f), p(8.5f, 7.0f), brush, stroke, roundStroke.Get());
                rt->DrawLine(p(8.5f, 7.0f), p(10.5f, 9.5f), brush, stroke, roundStroke.Get());
                rt->DrawLine(p(10.5f, 9.5f), p(20.5f, 9.5f), brush, stroke, roundStroke.Get());
                rt->DrawLine(p(20.5f, 9.5f), p(20.5f, 18.5f), brush, stroke, roundStroke.Get());
                rt->DrawLine(p(20.5f, 18.5f), p(3.5f, 18.5f), brush, stroke, roundStroke.Get());
                rt->DrawLine(p(3.5f, 18.5f), p(3.5f, 7.0f), brush, stroke, roundStroke.Get());
            }
            break;
        }

        // ── 20. 实线样式 ────────────────────────────────────────────────────────────
        case CaptureIconId::PropSolidLine: {
            float lx1 = rect.left + 5.0f * scale;
            float lx2 = rect.right - 5.0f * scale;
            rt->DrawLine(D2D1::Point2F(lx1, cy), D2D1::Point2F(lx2, cy), brush, stroke, roundStroke.Get());
            break;
        }

        // ── 21. 长虚线样式 ──────────────────────────────────────────────────────────
        case CaptureIconId::PropDashedLine: {
            float lx1 = rect.left + 4.0f * scale;
            float lx2 = rect.right - 4.0f * scale;
            float seg = 5.0f * scale, sp = 3.0f * scale;
            for (float sx = lx1; sx < lx2; sx += seg + sp) {
                rt->DrawLine(D2D1::Point2F(sx, cy), D2D1::Point2F(std::min(lx2, sx + seg), cy), brush, stroke, roundStroke.Get());
            }
            break;
        }

        // ── 22. 点虚线样式 ──────────────────────────────────────────────────────────
        case CaptureIconId::PropDottedLine: {
            float lx1 = rect.left + 4.0f * scale;
            float lx2 = rect.right - 4.0f * scale;
            float seg = 1.8f * scale, sp = 2.6f * scale;
            for (float sx = lx1; sx < lx2; sx += seg + sp) {
                rt->DrawLine(D2D1::Point2F(sx, cy), D2D1::Point2F(std::min(lx2, sx + seg), cy), brush, stroke, roundStroke.Get());
            }
            break;
        }

        // ── 23. 点划线样式 ──────────────────────────────────────────────────────────
        case CaptureIconId::PropDashDotLine: {
            float lx1 = rect.left + 4.0f * scale;
            float lx2 = rect.right - 4.0f * scale;
            float sx = lx1;
            while (sx < lx2) {
                rt->DrawLine(D2D1::Point2F(sx, cy), D2D1::Point2F(std::min(lx2, sx + 5.0f * scale), cy), brush, stroke, roundStroke.Get());
                sx += 7.5f * scale;
                if (sx < lx2) {
                    rt->DrawLine(D2D1::Point2F(sx, cy), D2D1::Point2F(std::min(lx2, sx + 1.8f * scale), cy), brush, stroke, roundStroke.Get());
                    sx += 4.0f * scale;
                }
            }
            break;
        }

        // ── 24. 线宽步进器图标 (三级递增阶梯横线) ──────────────────────────────────
        case CaptureIconId::PropStrokeWidth: {
            rt->DrawLine(p(4, 6), p(20, 6), brush, stroke * 0.7f, roundStroke.Get());
            rt->DrawLine(p(4, 12), p(20, 12), brush, stroke * 1.15f, roundStroke.Get());
            rt->DrawLine(p(4, 18), p(20, 18), brush, stroke * 1.75f, roundStroke.Get());
            break;
        }

        // ── 25. 圆角步进器图标 (优雅 90° 圆弧切角) ──────────────────────────────────
        case CaptureIconId::PropCornerRadius: {
            auto cornerArc = createPath({ p(5, 19), p(5, 12), p(6.5f, 8.5f), p(8.5f, 6.5f), p(12, 5), p(19, 5) }, false);
            if (cornerArc) {
                rt->DrawGeometry(cornerArc.Get(), brush, stroke, roundStroke.Get());
            } else {
                rt->DrawLine(p(5, 19), p(5, 12), brush, stroke, roundStroke.Get());
                rt->DrawLine(p(5, 12), p(6.5f, 8.5f), brush, stroke, roundStroke.Get());
                rt->DrawLine(p(6.5f, 8.5f), p(8.5f, 6.5f), brush, stroke, roundStroke.Get());
                rt->DrawLine(p(8.5f, 6.5f), p(12, 5), brush, stroke, roundStroke.Get());
                rt->DrawLine(p(12, 5), p(19, 5), brush, stroke, roundStroke.Get());
            }
            break;
        }

        // ── 26. 描边模式 (空心框) ───────────────────────────────────────────────────
        case CaptureIconId::PropFillOutline: {
            auto box = D2D1::RectF(cx - 7.5f * s, cy - 7.5f * s, cx + 7.5f * s, cy + 7.5f * s);
            rt->DrawRoundedRectangle(D2D1::RoundedRect(box, 2.0f * s, 2.0f * s), brush, stroke, roundStroke.Get());
            break;
        }

        // ── 27. 实心填充模式 (实心框) ───────────────────────────────────────────────
        case CaptureIconId::PropFillSolid: {
            auto box = D2D1::RectF(cx - 7.5f * s, cy - 7.5f * s, cx + 7.5f * s, cy + 7.5f * s);
            rt->FillRoundedRectangle(D2D1::RoundedRect(box, 2.0f * s, 2.0f * s), brush);
            break;
        }

        // ── 28. 取色吸管 (Lucide: Pipette) ──────────────────────────────────────────
        case CaptureIconId::PropPipette: {
            auto body = createPath({ p(19, 3), p(21, 5), p(12, 14), p(8, 18), p(3, 21), p(6, 16), p(10, 12), p(19, 3) }, true);
            if (body) {
                rt->DrawGeometry(body.Get(), brush, stroke, roundStroke.Get());
            } else {
                rt->DrawLine(p(19, 3), p(21, 5), brush, stroke, roundStroke.Get());
                rt->DrawLine(p(21, 5), p(12, 14), brush, stroke, roundStroke.Get());
                rt->DrawLine(p(19, 3), p(10, 12), brush, stroke, roundStroke.Get());
                rt->DrawLine(p(10, 12), p(6, 16), brush, stroke, roundStroke.Get());
                rt->DrawLine(p(6, 16), p(3, 21), brush, stroke, roundStroke.Get());
                rt->DrawLine(p(3, 21), p(8, 18), brush, stroke, roundStroke.Get());
                rt->DrawLine(p(8, 18), p(12, 14), brush, stroke, roundStroke.Get());
            }
            // 微液滴
            auto drop = D2D1::Ellipse(p(2, 22), 1.0f * s, 1.0f * s);
            rt->FillEllipse(drop, brush);
            break;
        }

        // ── 29. 调色板 (Lucide: Palette) ────────────────────────────────────────────
        case CaptureIconId::PropPalette: {
            auto palBox = D2D1::RectF(cx - 8.0f * s, cy - 8.0f * s, cx + 8.0f * s, cy + 8.0f * s);
            rt->DrawRoundedRectangle(D2D1::RoundedRect(palBox, 5.0f * s, 5.0f * s), brush, stroke, roundStroke.Get());
            // 4 色颜料微孔 (红、橙、绿、蓝)
            static const D2D1_COLOR_F palPigments[4] = {
                D2D1::ColorF(0.96f, 0.25f, 0.37f, 1.0f), // 红
                D2D1::ColorF(0.96f, 0.62f, 0.04f, 1.0f), // 橙
                D2D1::ColorF(0.06f, 0.73f, 0.51f, 1.0f), // 绿
                D2D1::ColorF(0.23f, 0.51f, 0.96f, 1.0f), // 蓝
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
            // 8 个饱满科技光谱色彩分段 (珊瑚红、曜石橙、明快黄、薄荷绿、青空蓝、科技蓝、紫罗兰、品红色)
            static const D2D1_COLOR_F spectrumColors[8] = {
                D2D1::ColorF(0.96f, 0.25f, 0.37f, 1.0f), // #F43F5E 红
                D2D1::ColorF(0.96f, 0.62f, 0.04f, 1.0f), // #F59E0B 橙
                D2D1::ColorF(0.92f, 0.70f, 0.03f, 1.0f), // #EAB308 黄
                D2D1::ColorF(0.06f, 0.73f, 0.51f, 1.0f), // #10B981 绿
                D2D1::ColorF(0.02f, 0.71f, 0.83f, 1.0f), // #06B6D4 青
                D2D1::ColorF(0.23f, 0.51f, 0.96f, 1.0f), // #3B82F6 蓝
                D2D1::ColorF(0.66f, 0.33f, 0.97f, 1.0f), // #A855F7 紫
                D2D1::ColorF(0.93f, 0.28f, 0.60f, 1.0f)  // #EC4899 粉红
            };

            const float ringR = 6.8f * s;
            const float dotR = 2.0f * s;

            // 绘制 8 个环状环绕的饱满微晶色点
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

            // 中心微晶加号 (+)
            const float crossLen = 2.2f * s;
            const float crossStroke = std::max(1.2f, 1.4f * scale);
            rt->DrawLine(D2D1::Point2F(cx - crossLen, cy), D2D1::Point2F(cx + crossLen, cy), brush, crossStroke, roundStroke.Get());
            rt->DrawLine(D2D1::Point2F(cx, cy - crossLen), D2D1::Point2F(cx, cy + crossLen), brush, crossStroke, roundStroke.Get());
            break;
        }

        // ── 30. 二维码 (Lucide: QrCode) ─────────────────────────────────────────────
        case CaptureIconId::PropQrCode: {
            // 定位框 1 (左上)
            rt->DrawRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(p(3, 3).x, p(3, 3).y, p(9, 9).x, p(9, 9).y), 1.0f * s, 1.0f * s), brush, stroke * 0.9f, roundStroke.Get());
            rt->FillRectangle(D2D1::RectF(p(5, 5).x, p(5, 5).y, p(7, 7).x, p(7, 7).y), brush);
            // 定位框 2 (右上)
            rt->DrawRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(p(15, 3).x, p(15, 3).y, p(21, 9).x, p(21, 9).y), 1.0f * s, 1.0f * s), brush, stroke * 0.9f, roundStroke.Get());
            rt->FillRectangle(D2D1::RectF(p(17, 5).x, p(17, 5).y, p(19, 7).x, p(19, 7).y), brush);
            // 定位框 3 (左下)
            rt->DrawRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(p(3, 15).x, p(3, 15).y, p(9, 21).x, p(9, 21).y), 1.0f * s, 1.0f * s), brush, stroke * 0.9f, roundStroke.Get());
            rt->FillRectangle(D2D1::RectF(p(5, 17).x, p(5, 17).y, p(7, 19).x, p(7, 19).y), brush);
            // 数据微像素
            rt->FillRectangle(D2D1::RectF(p(15, 15).x, p(15, 15).y, p(17, 17).x, p(17, 17).y), brush);
            rt->FillRectangle(D2D1::RectF(p(19, 15).x, p(19, 15).y, p(21, 17).x, p(21, 17).y), brush);
            rt->FillRectangle(D2D1::RectF(p(15, 19).x, p(15, 19).y, p(17, 21).x, p(17, 21).y), brush);
            rt->FillRectangle(D2D1::RectF(p(19, 19).x, p(19, 19).y, p(21, 21).x, p(21, 21).y), brush);
            break;
        }

        // ── 31. 美化外壳 (Sparkle Frame / CleanShot X Style) ─────────────────────────
        case CaptureIconId::ToolBeautyShell: {
            // 外层柔和衬底画布
            auto outerBox = D2D1::RectF(p(3, 4).x, p(3, 4).y, p(21, 20).x, p(21, 20).y);
            rt->DrawRoundedRectangle(D2D1::RoundedRect(outerBox, 3.0f * s, 3.0f * s), brush, stroke * 0.9f, roundStroke.Get());
            // 内层阴影截图画框
            auto innerBox = D2D1::RectF(p(6, 7).x, p(6, 7).y, p(18, 17).x, p(18, 17).y);
            rt->DrawRoundedRectangle(D2D1::RoundedRect(innerBox, 2.0f * s, 2.0f * s), brush, stroke, roundStroke.Get());
            // 右上角微晶高光星光 (Sparkle)
            rt->DrawLine(p(19, 1), p(19, 5), brush, stroke * 0.8f, roundStroke.Get());
            rt->DrawLine(p(17, 3), p(21, 3), brush, stroke * 0.8f, roundStroke.Get());
            break;
        }

        // ── 32. 锁定选区比例 (Aspect Ratio / Proportions) ───────────────────────────
        case CaptureIconId::PropAspectRatio: {
            // 宽屏矩形画框 (16:9 比例微缩画框)
            auto frameBox = D2D1::RectF(p(3, 6).x, p(3, 6).y, p(21, 18).x, p(21, 18).y);
            rt->DrawRoundedRectangle(D2D1::RoundedRect(frameBox, 2.0f * s, 2.0f * s), brush, stroke, roundStroke.Get());
            // 中心比例冒号 (:)
            rt->FillEllipse(D2D1::Ellipse(p(12, 10), 1.0f * s, 1.0f * s), brush);
            rt->FillEllipse(D2D1::Ellipse(p(12, 14), 1.0f * s, 1.0f * s), brush);
            break;
        }

        // ── 33. 方角序号微晶胶囊 (Rounded Square Number Badge) ─────────────────────
        case CaptureIconId::PropNumberSquare: {
            auto sqBox = D2D1::RectF(p(4, 4).x, p(4, 4).y, p(20, 20).x, p(20, 20).y);
            rt->DrawRoundedRectangle(D2D1::RoundedRect(sqBox, 4.0f * s, 4.0f * s), brush, stroke, roundStroke.Get());
            // 序号 1 骨架
            rt->DrawLine(p(10, 9), p(12, 7), brush, stroke, roundStroke.Get());
            rt->DrawLine(p(12, 7), p(12, 17), brush, stroke, roundStroke.Get());
            rt->DrawLine(p(9, 17), p(15, 17), brush, stroke, roundStroke.Get());
            break;
        }

        // ── 33. 文字描边 (Type T with Outline Halo) ──────────────────────────────
        case CaptureIconId::PropTextOutline: {
            auto halo = D2D1::RectF(cx - 7.5f * s, cy - 7.5f * s, cx + 7.5f * s, cy + 7.5f * s);
            rt->DrawRoundedRectangle(D2D1::RoundedRect(halo, 3.0f * s, 3.0f * s), brush, stroke * 0.75f, roundStroke.Get());
            rt->DrawLine(p(12, 7.5f), p(12, 16.5f), brush, stroke * 1.2f, roundStroke.Get());
            rt->DrawLine(p(7.5f, 7.5f), p(16.5f, 7.5f), brush, stroke * 1.2f, roundStroke.Get());
            break;
        }

        default:
            break;
    }
}

} // namespace tools3000::capture
