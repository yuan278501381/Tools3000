#pragma once

#include "capture/CaptureState.h"

namespace tools3000::capture {

/// Rebuilds the capture/record toolbar from one shared layout algorithm used by
/// both painting and hit testing. Rows wrap when high-DPI controls would exceed
/// the physical work surface.
void rebuildCaptureToolbar(CaptureState& state, const D2D1_RECT_F& selectionRect,
                           D2D1_SIZE_F surfaceSize);

/// 执行截图向区域录屏模式的原子转换与状态清理
void transitionToRecordMode(CaptureState& state);

/// 获取工具栏按钮的悬停提示文本
std::wstring tooltipForButton(const ToolbarButton& button, bool chinese);

/// 计算悬停提示浮层矩形（含双工具栏跨栏避让与侧边竖排圆钮水平弹出）
D2D1_RECT_F calculateTooltipRect(const D2D1_RECT_F& buttonRect, float tw, float th, float scale,
                                 D2D1_SIZE_F surfaceSize, const D2D1_RECT_F& selRect,
                                 const D2D1_RECT_F& secondaryRect,
                                 const D2D1_RECT_F& primaryRect = {},
                                 bool isVerticalSideBar = false);

/// 尺寸仪表舱弹出配置菜单（px / dp 切换、坐标开关、单位开关）单一事实源几何布局
struct SizeMenuLayout {
    D2D1_RECT_F menuRect{};
    D2D1_RECT_F segmentTrackRect{};
    D2D1_RECT_F pxSegmentRect{};
    D2D1_RECT_F dpSegmentRect{};
    D2D1_RECT_F formulaRect{};
    float dividerY = 0.0f;
    D2D1_RECT_F positionRowRect{};
    D2D1_RECT_F positionSwitchRect{};
    D2D1_RECT_F unitRowRect{};
    D2D1_RECT_F unitSwitchRect{};
};

/// 根据已定位的菜单外框计算内部各交互分区精确几何（供绘制与点击命中共享）
SizeMenuLayout computeSizeMenuLayoutFromRect(const D2D1_RECT_F& menuRect, float scale);

/// 根据尺寸胶囊 HUD 位置与画布边界计算完整尺寸配置菜单几何布局
SizeMenuLayout computeSizeMenuLayout(const D2D1_RECT_F& hudRect, D2D1_SIZE_F surfaceSize, float scale);

/// 悬停在调色盘按钮 (ChooseCustomColor / SelectColor) 上滚动滚轮时微调不透明度 (30..255, +-15) 或明度 (Ctrl+滚轮, +-6%)
void adjustPaletteColorByWheel(CaptureState& state, MarkupTool btnTool, int delta, bool adjustBrightness);

}  // namespace tools3000::capture
