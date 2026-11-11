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
#include "capture/NumberBadgeFormatter.h"
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
    Standard = 0,         // 经典几何实心尖角单向箭头 (Classic Solid Filled Triangle)
    Thin = 1,             // 优雅细线开放折角单向箭头 (Elegant Open Chevron)
    Tech = 2,             // 圆润科技/流线机翼型箭头 (Modern Swept Aerodynamic Wing)
    DoubleEnded = 3,      // 双向对称箭头 (Double-ended Arrow)
    CampaignMarch = 4,    // 历史地图「长征行军」宽幅渐粗燕尾立体箭头 (Tapered Ribbon March Arrow)
    MarchDoubleTrack = 5  // 历史地图「双线行军」平行双轨空心路线箭头 (Double-Track Hollow Route Arrow)
};

/// 局部放大镜引出线样式（对标 Nature / CVPR / ISO 工程图谱四大经典范式）
enum class MagnifierCalloutStyle {
    TangentFrustum = 0, // 学术双外切线 (CVPR / Nature: 源取景细框 + 中心微十字 + 两条外切展开细线，中间 100% 透明)
    ElbowLeader = 1,    // 工程 135° 折线 (Nature / Cell 图谱: 源取景细框 + 中心微十字 + 45°斜线转水平折线)
    DashedDirect = 2,   // ISO 精密虚线 (默认方案: 精密虚线取景框 + 中心微十字 + 细虚线直连)
    CenterPointer = 3,  // 单轴直指中心 (极简指针: 无源外框，单根细线 + 微箭头直指被放大中心靶点)
    COUNT = 4
};

/// 线条起点/终点独立端点样式 (对标 PixPin 10 种端点系统)
enum class LineCapStyle {
    None = 0,           // 1. 无端点
    OpenArrow = 1,      // 2. 细线开角箭头
    SolidTriangle = 2,  // 3. 实心平底三角箭头
    HollowTriangle = 3, // 4. 空心三角箭头 (内部镂空，主线在底边截断)
    StealthArrow = 4,   // 5. 燕尾内凹实心箭头
    SolidCircle = 5,    // 6. 实心圆点
    HollowCircle = 6,   // 7. 空心圆环 (内部镂空，主线在外缘截断)
    SolidDiamond = 7,   // 8. 实心菱形
    HollowDiamond = 8,  // 9. 空心菱形 (内部镂空，主线在顶点截断)
    BarStop = 9         // 10. T型垂直截止线
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

/// 采样画布指定矩形区域的平均感知亮度 (0.0f ~ 1.0f，支持 BGR/BGRA，自动裁剪越界区域)
float sampleCanvasRegionLuminance(const cv::Mat& canvas, const cv::Rect& roiRect);

/// 根据文字颜色与背景亮度计算高反差自适应文字描边颜色 (Auto 模式)
MarkupColor computeAdaptiveTextOutlineColor(const MarkupColor& textColor, float bgLuminance);

/// 解析最终应用的文字描边颜色（支持显式指定描边色或 Auto 背景感知自适应反色）
MarkupColor resolveTextOutlineColor(const MarkupColor& textColor,
                                    const MarkupColor& outlineColor,
                                    const cv::Mat& canvas,
                                    const cv::Rect& textRect,
                                    bool withBackdrop = false);


enum class HitArea {
    None = -1,
    Body = 0,
    LT = 1, T = 2, RT = 3, R = 4, RB = 5, B = 6, LB = 7, L = 8,
    CornerRadius = 9,
    FocusSource = 10,
    LensOffset = 11,
    TextMoveBorder = 12
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
    bool smoothCurve = false;
    bool hasCustomCaps = false;
    LineCapStyle startCap = LineCapStyle::None;
    LineCapStyle endCap = LineCapStyle::SolidTriangle;

    // 起点/终点（矩形/箭头/椭圆用）
    cv::Point startPt{0, 0};
    cv::Point endPt{0, 0};

    // 画笔轨迹点
    std::vector<cv::Point> penPoints;

    // 文本内容与描边体系
    std::string text;
    float fontSize = 16.0f;
    cv::Size textRenderSize{0, 0}; // 记录最后一次渲染的文本尺寸，用于包围盒
    bool textOutline = true;       // 文字/图形是否描边 (默认开启)
    MarkupColor textOutlineColor{0, 0, 0, 0}; // 描边/边框颜色: a == 0 为自适应高对比描边，其他为固定颜色
    float outlineThickness = 2.0f; // 边框/描边粗细 (0.0f ~ 12.0f; <= 0.05f 表示无边框)
    bool hasCustomOutline = false; // 是否显式设置过独立边框粗细或边框颜色
    size_t textCaret = 0;          // 原地编辑光标位置 (UTF-16 码元索引)
    size_t textSelAnchor = 0;      // 原地编辑选区锚点 (UTF-16 码元索引)
    uint32_t textLastCaretTick = 0;// 最近一次移动光标/输入时间戳 (用于移动期间常亮不闪烁)

    // 序列号值与缩放
    int numberValue = 0;
    float dpiScale = 1.0f;
    NumberBadgeShape numberShape = NumberBadgeShape::Circle;
    NumberFormat numberFormat = NumberFormat::Arabic;
    bool hasLeaderArrow = false; // 是否带有引出指向箭头 (Numbered Step Callout)

    // 马赛克块大小
    int mosaicBlockSize = 12;

    // 放大镜倍率与半轴尺寸（默认正圆 rx == ry；支持独立拉宽/拉高为椭圆或宽幅圆角胶囊）
    float magnifierScale = 2.0f;
    int magnifierRadius = 60;
    int magnifierRadiusY = 0; // <= 0 时自动跟随 magnifierRadius 保持正圆
    MagnifierCalloutStyle magnifierCalloutStyle = MagnifierCalloutStyle::DashedDirect;

    int effectiveMagnifierRx() const noexcept {
        return (std::max)(20, magnifierRadius);
    }
    int effectiveMagnifierRy() const noexcept {
        return (magnifierRadiusY > 0) ? (std::max)(20, magnifierRadiusY) : effectiveMagnifierRx();
    }

    // 聚光灯参数
    float spotlightDimAlpha = 0.55f; // 暗化区域的不透明度 (默认 55%，弱化页面同时保留背景可读性)
    bool spotlightEllipse = true;    // true=椭圆区域, false=矩形/圆角矩形区域
    bool spotlightShowBorder = false;// 是否绘制聚光灯边缘高亮描边

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
    static constexpr size_t kMaxUndoSteps = 50;

    /// 轻量级全矢量状态快照（仅含图元几何/样式元数据与序号进度，零整图拷贝）
    struct MarkupSnapshot {
        std::vector<MarkupElement> elements;
        std::array<int, static_cast<size_t>(NumberFormat::COUNT)> nextNumbers = {1, 1, 1, 1, 1};
    };

    MarkupEngine() = default;

    /// 设置底图（截图原图）。会清空所有标注与撤销栈（用于全新选区）。
    void setBaseImage(const cv::Mat& image);

    /// 仅替换底图，保留现有标注与撤销栈（用于选区二次调整后重裁底图）。
    void updateBaseImage(const cv::Mat& image);

    /// 平移所有标注（含撤销/重做栈）。用于选区移动/缩放时让标注跟随屏幕内容。
    void translateAll(int dx, int dy);

    /// 获取当前合成图（底图 + 所有标注）
    /// @param includeActiveHandles 是否绘制激活手柄 (预览传 true，复制/保存传 false 避免污染图片)
    cv::Mat getCompositeImage(bool includeActiveHandles = false) const;

    /// 查询与获取底图信息
    bool hasBaseImage() const noexcept { return !m_baseImage.empty(); }
    const cv::Mat& getBaseImage() const noexcept { return m_baseImage; }
    int baseWidth() const noexcept { return m_baseImage.cols; }
    int baseHeight() const noexcept { return m_baseImage.rows; }

    // ── 事务快照与无损序列化 ─────────────────────────────────────────────

    /// 捕获当前完整矢量快照（不修改撤销栈）
    MarkupSnapshot captureSnapshot() const;

    /// 在修改已有图元样式/几何前压入当前状态快照（支持最多 kMaxUndoSteps 步撤销）
    void pushUndoSnapshot();

    /// 提交外部预先捕获的事前快照（如拖拽平移/缩放结束后确认发生了实质位移再提交）
    void commitUndoSnapshot(MarkupSnapshot&& beforeState);

    /// 判断两组矢量图元状态在几何与样式上是否完全等价
    static bool elementsEquivalent(const std::vector<MarkupElement>& a,
                                   const std::vector<MarkupElement>& b) noexcept;
    static bool elementEquivalent(const MarkupElement& a, const MarkupElement& b) noexcept;

    /// 导出干净的矢量图元副本列表（剥离临时选中/编辑态，用于贴图二次无损标注）
    std::vector<MarkupElement> exportElements() const;

    /// 导入既有矢量图元列表并重建序号状态（清空历史撤销栈，作为新的二次编辑起点）
    void importElements(const std::vector<MarkupElement>& elems);

    // ── 元素操作 ─────────────────────────────────────────────────────────

    /// 添加标注元素
    MarkupElement* addElement(std::unique_ptr<MarkupElement> element);

    /// 撤销上一步标注事务（增/删/改/移/清全支持）
    bool undo();

    /// 重做下一步标注事务
    bool redo();

    /// UI affordances must be able to report these without attempting a
    /// mutation. This keeps disabled toolbar and UIA state truthful.
    bool canUndo() const noexcept { return !m_undoHistory.empty(); }
    bool canRedo() const noexcept { return !m_redoHistory.empty(); }

    /// 硬重置清除所有标注与撤销/重做栈（用于新选区会话初始化或取消选区）
    void clearAll();

    /// 可撤销地清空当前所有标注（用于工具栏 Clear 按钮与清空快捷键，支持 Ctrl+Z 一键恢复）
    bool clearElementsUndoable();

    /// 获取当前标注数量
    size_t elementCount() const { return m_elements.size(); }

    /// 获取所有图元元素列表
    const std::vector<std::unique_ptr<MarkupElement>>& elements() const noexcept { return m_elements; }

    /// 是否存在任何标注（含撤销/重做栈中可恢复的标注状态）
    bool hasAnyMarkup() const {
        return !m_elements.empty() || !m_undoHistory.empty() || !m_redoHistory.empty();
    }

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
    MarkupElement* drawNumberMark(cv::Point position, MarkupColor color, float dpiScale = 1.0f,
                                  NumberBadgeShape shape = NumberBadgeShape::Circle,
                                  NumberFormat format = NumberFormat::Arabic);

    /// 添加序列号标记 (返回序号数值，保持向后兼容)
    int addNumberMark(cv::Point position, MarkupColor color, float dpiScale = 1.0f,
                      NumberBadgeShape shape = NumberBadgeShape::Circle,
                      NumberFormat format = NumberFormat::Arabic);

    /// 添加放大镜
    MarkupElement* addMagnifier(cv::Point center, float scale = 2.0f, int radius = 60);

    /// 添加聚光灯（暗化选区外区域，突出选区内容）
    MarkupElement* addSpotlight(cv::Point p1, cv::Point p2, MarkupColor color, float dimAlpha = 0.55f, bool ellipse = true);

    /// 添加水印叠加（在选区内重复绘制旋转水印文字）
    MarkupElement* addWatermark(cv::Point p1, cv::Point p2, const std::string& text, float opacity = 0.15f, float angle = -30.0f);

    /// 智能消除（利用 cv::inpaint 重建选区背景）
    MarkupElement* applyInpaint(cv::Point p1, cv::Point p2, int radius = 5);

    /// 获取指定制式的当前待标序列号（5 种制式各自维护完全独立的递增序列）
    int currentNumber(NumberFormat format = NumberFormat::Arabic) const noexcept;

    /// 重置指定制式的序列号起始值 (默认为 1)
    void resetNumber(int start = 1, NumberFormat format = NumberFormat::Arabic) noexcept;

    /// 重置全部 5 种制式的序列号回初始值 1
    void resetAllNumbers() noexcept;

    /// 渲染单个元素到图像上
    void renderElement(cv::Mat& canvas, const MarkupElement& element, bool includeActiveHandles = false) const;

    /// 渲染所有元素
    void renderAll(cv::Mat& canvas, bool includeActiveHandles = false) const;

private:
    struct FilterRoiCacheEntry {
        MarkupTool tool = MarkupTool::Mosaic;
        cv::Rect dstRoi;
        int paramValue = 0;
        int channels = 0;
        uint64_t inputChecksum = 0;
        cv::Mat resultPatch;
    };

    static constexpr size_t numberFormatSlot(NumberFormat format) noexcept {
        const int idx = static_cast<int>(format);
        return (idx >= 0 && idx < static_cast<int>(NumberFormat::COUNT))
            ? static_cast<size_t>(idx)
            : 0u;
    }
    void recalculateNextNumbers() noexcept;
    void restoreSnapshot(const MarkupSnapshot& snapshot);
    void renderFilterElementCached(cv::Mat& canvas, const MarkupElement& elem) const;
    void renderSubset(cv::Mat& canvas, size_t count, bool includeActiveHandles) const;

    cv::Mat m_baseImage;                                    // 底图
    std::vector<std::unique_ptr<MarkupElement>> m_elements; // 当前标注元素
    std::deque<MarkupSnapshot> m_undoHistory;               // 撤销快照栈 (上限 kMaxUndoSteps)
    std::deque<MarkupSnapshot> m_redoHistory;               // 重做快照栈 (上限 kMaxUndoSteps)
    std::array<int, static_cast<size_t>(NumberFormat::COUNT)> m_nextNumbers = {1, 1, 1, 1, 1}; // 各制式独立序列号

    uint64_t m_baseRevision = 1;
    mutable std::unordered_map<uint32_t, FilterRoiCacheEntry> m_filterRoiCache;
    mutable cv::Mat m_underlayCache;
    mutable uint64_t m_underlayBaseRev = 0;
    mutable std::vector<MarkupElement> m_underlayElements;
};

}  // namespace tools3000::capture
