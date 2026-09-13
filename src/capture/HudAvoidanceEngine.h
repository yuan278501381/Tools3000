#pragma once
// ─────────────────────────────────────────────────────────────────────────────
// HudAvoidanceEngine.h — 通用浮动 HUD / 标牌智能几何自动躲避求解引擎
//
// 架构定位与核心职责:
//   1. 提供高内聚、纯几何、完全可复用的框架级 HUD 放置决策求解器；
//   2. 支持选区尺寸胶囊、色彩放大镜、工具栏、微晶提示气泡等各类浮层；
//   3. 统一多障碍物感知：光标安全缓冲区、主/二级工具栏、图元标注框、手柄热区；
//   4. 综合成本评估算法：边缘偏好、视口边界越界惩罚、障碍物重叠面积积分惩罚；
//   5. 状态迟滞与防抖（Hysteresis & Anti-Jitter）：引入锚点历史稳定性奖励分，彻底消灭临界跳闪。
//
// 版权声明: Copyright (c) 2026 Yy1 (yuan278501381). All rights reserved.
// ─────────────────────────────────────────────────────────────────────────────

#ifndef TOOLS3000_CAPTURE_HUDAVOIDANCEENGINE_H
#define TOOLS3000_CAPTURE_HUDAVOIDANCEENGINE_H

#include <d2d1.h>
#include <vector>
#include <optional>
#include <cstdint>

namespace tools3000::capture {

/// 障碍物几何类型
enum class HudObstacleType {
    Cursor = 0,           ///< 鼠标光标及周边交互安全缓冲区
    PrimaryToolbar,       ///< 主操作工具栏
    SecondaryToolbar,     ///< 二级属性工具栏
    SelectionHandle,      ///< 选区 8 向调节把手或圆角控制器
    ActiveAnnotation,     ///< 正在绘制或选中的图元标注框
    CustomRect            ///< 外部自定义避让矩形
};

/// 障碍物描述符
struct HudObstacle {
    D2D1_RECT_F rect{};
    HudObstacleType type = HudObstacleType::CustomRect;
    float weight = 1.0f;  ///< 惩罚权重因子
};

/// HUD 候选挂载边缘锚点
enum class HudAnchorEdge {
    TopLeft = 0,         ///< 选区外侧左上（默认黄金视觉位）
    TopRight,            ///< 选区外侧右上
    BottomLeft,          ///< 选区外侧左下
    BottomRight,         ///< 选区外侧右下
    TopCenter,           ///< 选区外侧居中靠上
    BottomCenter,        ///< 选区外侧居中靠下
    InsideTopLeft,       ///< 选区内侧左上（外部全满或贴顶时的优雅内嵌备选）
    InsideTopRight,      ///< 选区内侧右上
    InsideBottomLeft,    ///< 选区内侧左下
    InsideBottomRight,   ///< 选区内侧右下
    InsideTopCenter,     ///< 选区内侧居中靠上
    InsideBottomCenter,  ///< 选区内侧居中靠下
    OuterLeftTop,        ///< 选区外侧左上侧边 (窄选区或上下遮挡时的侧边逃逸候选)
    OuterLeftCenter,     ///< 选区外侧左侧居中
    OuterRightTop,       ///< 选区外侧右上侧边
    OuterRightCenter     ///< 选区外侧右侧居中
};

/// 求解引擎配置参数
struct HudPlacementConfig {
    float edgeGap = 6.0f;               ///< 离选区边缘的基础间距 (px)
    float screenMargin = 8.0f;          ///< 离显示器/工作区边界的最小间距 (px)
    float cursorSafeRadius = 26.0f;     ///< 光标防遮挡安全半径 (px)
    float stabilityBonus = 45.0f;       ///< 维持上一帧候选位的迟滞奖励分 (防抖动)
    bool allowInsideCandidates = true;  ///< 是否允许内嵌至选区内部
    bool avoidCursor = true;            ///< 是否启用光标避让
};

/// 求解结果
struct HudPlacementResult {
    D2D1_RECT_F rect{};                  ///< 计算出的最优放置矩形
    HudAnchorEdge edge = HudAnchorEdge::TopLeft; ///< 命中的候选锚点
    bool isInside = false;               ///< 是否处于选区内部
    float cost = 0.0f;                   ///< 综合代价值
};

/// 通用 HUD 自动躲避求解引擎
class HudAvoidanceEngine {
public:
    /// 核心求解函数：在给定锚点选区与动态障碍物集合下，计算最优 HUD 矩形
    /// @param anchorRect 锚点矩形（如选区 selRect）
    /// @param hudWidth HUD 自身物理像素宽度
    /// @param hudHeight HUD 自身物理像素高度
    /// @param surfaceSize 当前渲染平面/屏幕尺寸
    /// @param obstacles 动态感知到的障碍物列表
    /// @param config 求解配置参数
    /// @param scale 当前显示器物理 DPI 缩放比例
    /// @param previousEdge 上一帧采纳的候选边缘（用于迟滞防抖，-1 代表无历史）
    /// @return 最优放置结果
    static HudPlacementResult solvePlacement(
        const D2D1_RECT_F& anchorRect,
        float hudWidth,
        float hudHeight,
        D2D1_SIZE_F surfaceSize,
        const std::vector<HudObstacle>& obstacles,
        const HudPlacementConfig& config,
        float scale = 1.0f,
        int previousEdge = -1);

    /// 计算给定边缘的原生候选矩形（未裁剪前）
    static D2D1_RECT_F candidateRectForEdge(
        const D2D1_RECT_F& anchorRect,
        float hudWidth,
        float hudHeight,
        HudAnchorEdge edge,
        float gap,
        float scale);

    /// 两个 D2D1 矩形是否相交
    static bool rectsIntersect(const D2D1_RECT_F& a, const D2D1_RECT_F& b);

    /// 计算两个 D2D1 矩形的相交面积
    static float intersectionArea(const D2D1_RECT_F& a, const D2D1_RECT_F& b);

    /// 将矩形约束在视口边界内
    static D2D1_RECT_F clampToSurface(
        const D2D1_RECT_F& rect,
        D2D1_SIZE_F surfaceSize,
        float margin);
};

} // namespace tools3000::capture

#endif // TOOLS3000_CAPTURE_HUDAVOIDANCEENGINE_H
