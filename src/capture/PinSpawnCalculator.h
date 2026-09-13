#pragma once
// ─────────────────────────────────────────────────────────────────────────────
// PinSpawnCalculator.h — 世界级贴图智能生成坐标与碰撞避让几何引擎
//
// 架构职责与算法契约:
//   1. 还原截屏原位 (In-Place Restore)：若贴图来源于截图历史或指定选区，
//      100% 精确放置在原截图屏幕坐标 (region.x, region.y)；
//   2. 美化外壳 Padding 逆向对齐：若图片带有美化外壳 padding（imageWidth > region.width），
//      自动向外反向平移 padding 像素，确保被截取的实际屏幕内容像素级绝对原位对齐；
//   3. 外部内容智能视口居中吸附：非截图或无历史匹配的外部图片/颜色/文本，
//      以当前鼠标光标为中心居中展开，并强制夹取在当前显示器工作区 (WorkArea) 内，
//      杜绝被 Windows 任务栏或屏幕外切角遮挡；
//   4. 优雅级联防完全重叠 (Smart Cascading)：若在目标坐标已存在活跃贴图窗口，
//      自动按 +24px 阶梯向右下级联避让，并在触碰视口边界时优雅向内收缩。
//
// 版权声明: Copyright (c) 2026 Yy1 (yuan278501381). All rights reserved.
// ─────────────────────────────────────────────────────────────────────────────

#ifndef TOOLS3000_CAPTURE_PINSPAWNCALCULATOR_H
#define TOOLS3000_CAPTURE_PINSPAWNCALCULATOR_H

#include <windows.h>
#include <vector>
#include <optional>
#include <cmath>
#include <algorithm>

namespace tools3000::capture {

/// 选区坐标描述
struct PinSpawnRegion {
    int x = 0;
    int y = 0;
    int width = 0;
    int height = 0;
};

/// 截图历史快照（用于原位比对）
struct PinHistorySnapshot {
    int imageWidth = 0;
    int imageHeight = 0;
    PinSpawnRegion region{};
};

/// 求解输入参数
struct PinSpawnInput {
    int imageWidth = 0;
    int imageHeight = 0;
    int padX = 0;
    int padY = 0;
    POINT cursor{0, 0};
    RECT workArea{0, 0, 1920, 1080};
    RECT virtualScreen{0, 0, 1920, 1080};
    std::optional<PinSpawnRegion> preferredRegion{};
    std::optional<PinHistorySnapshot> lastHistory{};
    std::vector<RECT> existingPins{};
    int cascadeOffset = 24;
};

/// 求解计算器
class PinSpawnCalculator {
public:
    /// 计算最佳生成坐标
    static POINT calculate(const PinSpawnInput& input);
};

} // namespace tools3000::capture

#endif // TOOLS3000_CAPTURE_PINSPAWNCALCULATOR_H
