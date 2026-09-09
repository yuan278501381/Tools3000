#pragma once
// ─────────────────────────────────────────────────────────────────────────────
// CornerRadiusHelper.h — 跨选区/美化外壳/矩形标注通用圆角手柄交互框架
//
// 职责:
//   1. 统一管理内角控制手柄几何坐标与自适应动态偏移算法
//   2. 统一实现 Figma 纯净隐形流近场感应渐显与精准锚点命中检测
//   3. 统一推拉数学算法 (往几何中心拉增大圆角，往外推减小圆角)
//   4. 统一世界级微晶像素反馈 Toast 文本格式化
// ─────────────────────────────────────────────────────────────────────────────

#ifndef TOOLS3000_CAPTURE_CORNER_RADIUS_HELPER_H
#define TOOLS3000_CAPTURE_CORNER_RADIUS_HELPER_H

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <algorithm>
#include <cmath>
#include <string>
#include <vector>

namespace tools3000::capture {

/// 圆角调节应用目标实体
enum class CornerRadiusTarget {
    Selection,       // 选区框 (Selection Box)
    BeautifyShell,   // 美化外壳 (Beauty Shell)
    RectangleMarkup  // 矩形标注 (Rectangle Markup)
};

/// 内角控制点几何信息
struct CornerHandle {
    float x = 0.0f;
    float y = 0.0f;
    float inDirX = 0.70710678f;  // 对角内向单位向量 X (指向矩形中心)
    float inDirY = 0.70710678f;  // 对角内向单位向量 Y
    int cornerIndex = 0;         // 0=LT, 1=RT, 2=RB, 3=LB
};

/// Figma 纯净隐形流：近场感应渐显状态
struct CornerProximityState {
    int activeCornerIndex = -1;     // 当前唤醒的角索引 (-1 表示完全隐形，0 内部噪点)
    float visibilityAlpha = 0.0f;   // 渐显不透明度 [0.0, 1.0]
    bool isDirectHover = false;     // 是否位于手柄直接可点击热区内（触发完整高光与 R 胶囊）
};

/// 框架级圆角交互辅助引擎
class CornerRadiusHelper {
public:
    static constexpr float kProximityRevealRadiusLogical = 56.0f;
    static constexpr float kDirectHoverRadiusLogical = 13.0f;

    /// 判断是否满足显示内侧圆角调节手柄的最小尺寸条件
    /// @param width 矩形宽度
    /// @param height 矩形高度
    /// @param scale DPI 缩放倍率
    /// @param minDimension 最小包围盒阈值
    static bool canShowHandles(float width, float height, float scale = 1.0f, float minDimension = 48.0f);

    /// 计算内角手柄距边界的内向动态偏移量
    static float calculateOffset(float cornerRadius, float width, float height, float scale = 1.0f,
                                 float minOffsetLogical = 16.0f, float baseMinLogical = 22.0f);

    /// 获取所有有效内角控制点坐标与对角导轨单位向量
    /// @param singlePrimaryCorner 当包围盒较小时，自动收敛为唯一主手柄 (左上角)，杜绝临界跳动
    static std::vector<CornerHandle> getCornerHandles(
        float left, float top, float right, float bottom,
        float cornerRadius, float scale = 1.0f, bool singlePrimaryCorner = false);

    /// 计算鼠标相对四角手柄的近场感应渐显状态（平时远离角部返回 alpha=0 完全隐形）
    static CornerProximityState evaluateProximity(
        const std::vector<CornerHandle>& handles,
        float cursorX, float cursorY,
        float scale = 1.0f,
        bool isDragging = false,
        int dragCornerIndex = -1,
        float dragStartX = 0.0f,
        float dragStartY = 0.0f) noexcept;

    /// 命中测试：检测指定坐标是否命中任意圆角手柄
    /// @return 命中的角索引 (0..3)，未命中返回 -1
    static int hitTestHandles(
        float left, float top, float right, float bottom,
        float cornerRadius, float px, float py,
        float scale = 1.0f, float hitRadius = 11.0f, bool singlePrimaryCorner = false);

    /// 统一拖拽推拉数学算法：根据鼠标当前位移计算最新圆角半径
    /// 核心法则：往几何中心拉（内向）增加圆角，往几何外角推（外向）减小圆角
    static float calculateDraggedRadius(
        float left, float top, float right, float bottom,
        float dragStartX, float dragStartY,
        float startRadius,
        float currentCursorX, float currentCursorY,
        float maxRadiusLimit = 0.0f,
        int cornerIndex = -1,
        float scale = 1.0f);

    /// 增量式单步拖拽算法 (适用于通过 (dx, dy) 相对偏移累加的场景)
    static float applyIncrementalDrag(
        float currentRadius,
        float dx, float dy,
        float signX, float signY,
        float maxRadius);

    /// 获取统一世界级微晶 Toast 像素反馈文本
    static std::wstring getToastMessage(CornerRadiusTarget target, float radius, bool isZh);

    /// 获取或生成高分屏感知、黄金比例 (22px 视觉跨度) 的圆角调节专用硬件光标
    static HCURSOR getLiveCursor(int cornerIndex, float dpiScale = 1.0f, bool isDragging = false);
};

} // namespace tools3000::capture

#endif // TOOLS3000_CAPTURE_CORNER_RADIUS_HELPER_H
