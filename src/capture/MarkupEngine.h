#pragma once
// ─────────────────────────────────────────────────────────────────────────────
// MarkupEngine — 截图标注引擎
//
// 职责:
//   1. 管理所有标注工具 (矩形/箭头/椭圆/画笔/高亮/马赛克/文本/放大镜/序号/聚光灯/水印/智能消除)
//   2. 维护标注元素列表（支持撤销/重做）
//   3. 使用 OpenCV 将标注渲染到截图上
//   4. 自动序号递增管理
// ─────────────────────────────────────────────────────────────────────────────

#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <algorithm>
#include <opencv2/opencv.hpp>
#include <string>
#include <vector>
#include <memory>
#include <cstdint>
#include <deque>
#include <unordered_map>
#include <array>

namespace tools3000::capture {

/// GDI+ must be started and stopped explicitly while the capture DLL is fully
/// loaded. Starting it from a DLL global constructor makes GdiplusShutdown run
/// under the Windows loader lock and can deadlock on its background thread.
bool initializeMarkupTextRenderer();
void shutdownMarkupTextRenderer();

/// 图像圆角裁切处理 (保留平滑抗锯齿 Alpha 透明通道)
cv::Mat applyRoundedCorners(const cv::Mat& src, float radius);

/// 标注工具类型
enum class MarkupTool {
    Rectangle,   // 矩形
    Line,        // 直线 (Snipaste / PixPin 核心基础工具)
    Arrow,       // 箭头
    Ellipse,     // 椭圆
    Pen,         // 画笔
    Highlight,   // 高亮
    Mosaic,      // 马赛克
    Text,        // 文本
    Magnifier,   // 放大镜
    Number,      // 序列号标记
    Spotlight,   // 聚光灯（暗化选区外区域）
    Watermark,   // 水印叠加
    Inpaint,     // 智能消除（背景重建）
    Blur,        // 平滑高斯模糊
};

/// 线条样式
enum class LineStyle {
    Solid = 0,   // 实线 ──────
    Dashed = 1,  // 长虚线 ------
    Dotted = 2,  // 点虚线 ......
    DashDot = 3, // 点划线 -.-.-.-
};

/// 箭头样式
enum class ArrowStyle {
    Standard = 0,   // 经典几何实心尖角单向箭头 (Classic Solid Filled Triangle)
    Thin = 1,       // 优雅细线开放折角单向箭头 (Elegant Open Chevron)
    Tech = 2,       // 圆润科技/流线机翼型箭头 (Modern Swept Aerodynamic Wing)
    DoubleEnded = 3 // 双向对称箭头 (Double-ended Arrow)
};

/// 序号标号形状
enum class NumberBadgeShape {
    Circle = 0,       // 经典圆圈标号
    RoundedSquare = 1 // 现代方角微晶胶囊
};

/// 颜色预设
struct MarkupColor {
    uint8_t r, g, b, a;

    cv::Scalar toCvScalar() const { return cv::Scalar(b, g, r, a); }

    static MarkupColor Red()    { return {244, 63, 94, 255}; }   // #F43F5E 珊瑚红
    static MarkupColor Orange() { return {245, 158, 11, 255}; }  // #F59E0B 曜石橙
    static MarkupColor Yellow() { return {234, 179, 8, 255}; }   // #EAB308 明快黄
    static MarkupColor Green()  { return {16, 185, 129, 255}; }  // #10B981 薄荷绿
    static MarkupColor Blue()   { return {59, 130, 246, 255}; }  // #3B82F6 科技蓝
    static MarkupColor Black()  { return {30, 41, 59, 255}; }    // #1E293B 极客黑
    static MarkupColor White()  { return {255, 255, 255, 255}; } // #FFFFFF 纯白
    static MarkupColor Purple() { return {139, 92, 246, 255}; }
    static MarkupColor Auto()   { return {0, 0, 0, 0}; }          // 0 Alpha 代表自适应反色/高对比

    bool operator==(const MarkupColor& other) const {
        return r == other.r && g == other.g && b == other.b && a == other.a;
    }
    bool operator!=(const MarkupColor& other) const {
        return !(*this == other);
    }
    bool isAuto() const { return a == 0; }
};

enum class HitArea {
    None = -1,
    Body = 0,
    LT = 1, T = 2, RT = 3, R = 4, RB = 5, B = 6, LB = 7, L = 8,
    CornerRadius = 9
};

struct MarkupElement;

struct HitResult {
    MarkupElement* element = nullptr;
    HitArea area = HitArea::None;
};

/// 标注元素基类
struct MarkupElement {
    MarkupTool tool;
    MarkupColor color = MarkupColor::Red();
    float thickness = 2.0f;
    LineStyle lineStyle = LineStyle::Solid;
    bool fill = false;
    float cornerRadius = 0.0f;
    ArrowStyle arrowStyle = ArrowStyle::Standard;

    // 起点/终点（矩形/箭头/椭圆用）
    cv::Point startPt{0, 0};
    cv::Point endPt{0, 0};

    // 画笔轨迹点
    std::vector<cv::Point> penPoints;

    // 文本内容与描边体系
    std::string text;
    float fontSize = 16.0f;
    cv::Size textRenderSize{0, 0}; // 记录最后一次渲染的文本尺寸，用于包围盒
    bool textOutline = true;       // 文字是否描边 (默认开启)
    MarkupColor textOutlineColor{0, 0, 0, 0}; // 描边颜色: a == 0 为自适应高对比描边，其他为固定颜色

    // 序列号值与缩放
    int numberValue = 0;
    float dpiScale = 1.0f;
    NumberBadgeShape numberShape = NumberBadgeShape::Circle;
    bool hasLeaderArrow = false; // 是否带有引出指向箭头 (Numbered Step Callout)

    // 马赛克块大小
    int mosaicBlockSize = 12;

    // 放大镜倍率
    float magnifierScale = 2.0f;
    int magnifierRadius = 60;

    // 聚光灯参数
    float spotlightDimAlpha = 0.6f;  // 暗化区域的不透明度
    bool spotlightEllipse = true;     // true=椭圆区域, false=矩形区域

    // 水印参数
    std::string watermarkText;        // 水印文字
    float watermarkOpacity = 0.15f;   // 水印透明度
    float watermarkAngle = -30.0f;    // 旋转角度（度）
    int watermarkSpacing = 120;       // 水印间距

    // 智能消除（Inpaint）参数
    int inpaintRadius = 5;            // 修复半径

    uint32_t id = 0; // 唯一标识

    // 交互状态
    bool isActive = false;
    bool isEditing = false;

    virtual ~MarkupElement() = default;

    cv::Rect getBoundingBox() const;
    HitArea hitTestEx(cv::Point pt, int padding = 5) const;
    bool hitTest(cv::Point pt, int padding = 5) const { return hitTestEx(pt, padding) != HitArea::None; }
    void moveBy(int dx, int dy);
    void resize(int dx, int dy, HitArea handle);
};

/// 多态标注工具策略接口 (Polymorphic Tool Handler Interface)
class IMarkupToolHandler {
public:
    virtual ~IMarkupToolHandler() = default;
    virtual void render(cv::Mat& canvas, const MarkupElement& element) const = 0;
    virtual cv::Rect getBoundingBox(const MarkupElement& element) const = 0;
    virtual HitArea hitTest(const MarkupElement& element, cv::Point pt, int padding) const = 0;
    virtual void resize(MarkupElement& element, int dx, int dy, HitArea handle) const = 0;
    virtual void renderActiveHandles(cv::Mat& canvas, const MarkupElement& element) const = 0;
};

/// 8 方向控制手柄几何与命中管理引擎 (Single-Responsibility Handle Geometry Engine)
class HandleGeometry {
public:
    static constexpr int kDefaultHandleHalfSize = 7;
    static constexpr int kDefaultMinBoxSize = 4;

    struct HandlePoint {
        cv::Point point;
        HitArea area;
    };

    /// 计算给定包围盒的 8 个手柄位置与对应 HitArea (LT, T, RT, R, RB, B, LB, L)
    static std::array<HandlePoint, 8> getBoxHandles(const cv::Rect& bbox);

    /// 对给定包围盒的 8 方向手柄进行命中测试 (优先角手柄，后边手柄)
    static HitArea hitTestHandles(const cv::Rect& bbox, cv::Point pt, int handleHalfSize = kDefaultHandleHalfSize);

    /// 在画布上统一渲染 8 方向激活手柄 (外圈纯白高反差 + 细边框 + 微晶阴影，与桌面质感完全对齐)
    static void renderBoxHandles(cv::Mat& canvas, const cv::Rect& bbox,
                                 const cv::Scalar& activeColor = cv::Scalar(255, 140, 0),
                                 int handleHalfSize = 5);

    /// 计算 8 方向手柄拖拽后的包围盒几何，保证尺寸不低于 minW / minH
    static cv::Rect computeResizedRect(const cv::Rect& originalRect, int dx, int dy, HitArea handle,
                                       int minW = kDefaultMinBoxSize, int minH = kDefaultMinBoxSize);

    /// 计算 8 方向手柄在鼠标微移 (dx, dy) 下的标量伸缩分量 (统一用于文字、放大镜等等比缩放图元)
    static int computeScalarDelta(int dx, int dy, HitArea handle);

    /// 计算在尺寸增量 (dW, dH) 下，根据被拖拽手柄保持对应锚点边/中心不动的新左上角坐标 (多态等比图元通用)
    static cv::Point computeAnchoredOrigin(const cv::Point& currentOrigin, int dW, int dH, HitArea handle);
};

/// 通用包围盒标注工具基类 (Default Bounding Box Tool Handler Base)
class DefaultBoxHandler : public IMarkupToolHandler {
public:
    cv::Rect getBoundingBox(const MarkupElement& element) const override;
    HitArea hitTest(const MarkupElement& element, cv::Point pt, int padding) const override;
    void resize(MarkupElement& element, int dx, int dy, HitArea handle) const override;
    void renderActiveHandles(cv::Mat& canvas, const MarkupElement& element) const override;

protected:
    /// 辅助方法：统一对手柄进行命中测试
    HitArea hitTestHandles(const MarkupElement& element, cv::Point pt, int padding) const;
};

/// 标注工具单例注册表 (Open-Closed Extensible Tool Registry)
class MarkupToolRegistry {
public:
    static MarkupToolRegistry& instance();
    void registerHandler(MarkupTool tool, std::shared_ptr<IMarkupToolHandler> handler);
    const IMarkupToolHandler* getHandler(MarkupTool tool) const;

private:
    MarkupToolRegistry();
    std::unordered_map<MarkupTool, std::shared_ptr<IMarkupToolHandler>> m_handlers;
};

/// 标注引擎
class MarkupEngine {
public:
    MarkupEngine() = default;

    /// 设置底图（截图原图）。会清空所有标注与撤销栈（用于全新选区）。
    void setBaseImage(const cv::Mat& image);

    /// 仅替换底图，保留现有标注与撤销栈（用于选区二次调整后重裁底图）。
    void updateBaseImage(const cv::Mat& image);

    /// 平移所有标注（含撤销栈）。用于选区移动/缩放时让标注跟随屏幕内容。
    void translateAll(int dx, int dy);

    /// 获取当前合成图（底图 + 所有标注）
    /// @param includeActiveHandles 是否绘制激活手柄 (预览传 true，复制/保存传 false 避免污染图片)
    cv::Mat getCompositeImage(bool includeActiveHandles = false) const;

    /// 查询与获取底图信息
    bool hasBaseImage() const noexcept { return !m_baseImage.empty(); }
    const cv::Mat& getBaseImage() const noexcept { return m_baseImage; }
    int baseWidth() const noexcept { return m_baseImage.cols; }
    int baseHeight() const noexcept { return m_baseImage.rows; }

    // ── 元素操作 ─────────────────────────────────────────────────────────

    /// 添加标注元素
    MarkupElement* addElement(std::unique_ptr<MarkupElement> element);

    /// 撤销最后一个标注
    bool undo();

    /// 重做
    bool redo();

    /// UI affordances must be able to report these without attempting a
    /// mutation. This keeps disabled toolbar and UIA state truthful.
    bool canUndo() const noexcept { return !m_elements.empty(); }
    bool canRedo() const noexcept { return !m_undoStack.empty(); }

    /// 清除所有标注
    void clearAll();

    /// 获取当前标注数量
    size_t elementCount() const { return m_elements.size(); }

    /// 获取所有图元元素列表
    const std::vector<std::unique_ptr<MarkupElement>>& elements() const noexcept { return m_elements; }

    /// 是否存在任何标注（含撤销栈中已撤销但可重做的元素）
    bool hasAnyMarkup() const { return !m_elements.empty() || !m_undoStack.empty(); }

    /// 删除指定元素
    void removeElement(uint32_t id);

    /// 获取位于某点的元素及其命中区域（倒序查找，即最顶层的元素）
    HitResult getElementAtEx(cv::Point pt, int padding = 5) const;

    /// 获取位于某点的元素
    MarkupElement* getElementAt(cv::Point pt, int padding = 5) const {
        return getElementAtEx(pt, padding).element;
    }

    /// 获取指定 ID 的元素
    MarkupElement* getElementById(uint32_t id) const;

    // ── 工具快捷方法 ─────────────────────────────────────────────────────

    /// 画矩形
    MarkupElement* drawRectangle(cv::Point p1, cv::Point p2, MarkupColor color, float thickness = 2.0f);

    /// 画直线 (Snipaste / PixPin 核心标注)
    MarkupElement* drawLine(cv::Point p1, cv::Point p2, MarkupColor color, float thickness = 2.0f, LineStyle style = LineStyle::Solid);

    /// 画箭头
    MarkupElement* drawArrow(cv::Point from, cv::Point to, MarkupColor color, float thickness = 2.0f);

    /// 画椭圆
    MarkupElement* drawEllipse(cv::Point p1, cv::Point p2, MarkupColor color, float thickness = 2.0f);

    /// 画笔自由绘制
    MarkupElement* drawPenStroke(const std::vector<cv::Point>& points, MarkupColor color, float thickness = 2.0f);

    /// 高亮（半透明矩形）
    MarkupElement* drawHighlight(cv::Point p1, cv::Point p2, MarkupColor color);

    /// 马赛克区域
    MarkupElement* applyMosaic(cv::Point p1, cv::Point p2, int blockSize = 12);

    /// 平滑高斯模糊区域
    MarkupElement* applyBlur(cv::Point p1, cv::Point p2, int kernelSize = 15);

    /// 添加文本
    MarkupElement* addText(cv::Point position, const std::string& text, MarkupColor color, float fontSize = 16.0f,
                           bool textOutline = true, MarkupColor outlineColor = MarkupColor::Auto());

    /// 添加序列号标记 (返回元素指针)
    MarkupElement* drawNumberMark(cv::Point position, MarkupColor color, float dpiScale = 1.0f, NumberBadgeShape shape = NumberBadgeShape::Circle);

    /// 添加序列号标记 (返回序号数值，保持向后兼容)
    int addNumberMark(cv::Point position, MarkupColor color, float dpiScale = 1.0f, NumberBadgeShape shape = NumberBadgeShape::Circle);

    /// 添加放大镜
    void addMagnifier(cv::Point center, float scale = 2.0f, int radius = 60);

    /// 添加聚光灯（暗化选区外区域，突出选区内容）
    void addSpotlight(cv::Point p1, cv::Point p2, MarkupColor color, float dimAlpha = 0.6f, bool ellipse = true);

    /// 添加水印叠加（在选区内重复绘制旋转水印文字）
    void addWatermark(cv::Point p1, cv::Point p2, const std::string& text, float opacity = 0.15f, float angle = -30.0f);

    /// 智能消除（利用 cv::inpaint 重建选区背景）
    void applyInpaint(cv::Point p1, cv::Point p2, int radius = 5);

    /// 获取当前序列号
    int currentNumber() const { return m_nextNumber; }

    /// 重置序列号起始值 (默认为 1)
    void resetNumber(int start = 1) { m_nextNumber = (std::max)(1, start); }

    /// 渲染单个元素到图像上
    void renderElement(cv::Mat& canvas, const MarkupElement& element, bool includeActiveHandles = false) const;

    /// 渲染所有元素
    void renderAll(cv::Mat& canvas, bool includeActiveHandles = false) const;

private:
    cv::Mat m_baseImage;                                    // 底图
    std::vector<std::unique_ptr<MarkupElement>> m_elements; // 标注元素
    std::deque<std::unique_ptr<MarkupElement>> m_undoStack; // 撤销栈
    int m_nextNumber = 1;                                   // 下一个序列号
};

}  // namespace tools3000::capture
