#pragma once
#ifndef TOOLS3000_CAPTURE_CAPTUREDROPDOWNMENU_H
#define TOOLS3000_CAPTURE_CAPTUREDROPDOWNMENU_H

#include "capture/CaptureState.h"

namespace tools3000::capture {

/// 打开或轮转多态微晶下拉菜单
void openDropdownMenu(CaptureState& state, DropdownType type, const D2D1_RECT_F& anchorRect);

/// 提交下拉选项变更并写入持久化配置 (ConfigManager)
void commitDropdownSelection(CaptureState& state, DropdownType type, int id);

/// 更新下拉选项悬停状态及实时质感预览 (Hover Preview)
void updateDropdownHover(CaptureState& state, int hoverIndex);

/// 关闭下拉菜单并根据需要恢复预览前的原始值
void closeDropdownMenu(CaptureState& state, bool restorePreview = true);

/// 判断当前是否有任意截图悬浮弹出面板（下拉菜单 / 无级滑块 / 样式子菜单 / 尺寸配置菜单）处于展开态
bool hasAnyCapturePopupOpen(const CaptureState& state) noexcept;

/// 原子收起全部截图悬浮弹出面板，确保同屏严格最多只有一个弹出层
void closeAllCapturePopups(CaptureState& state, bool restoreDropdownPreview = true);

}  // namespace tools3000::capture

#endif  // TOOLS3000_CAPTURE_CAPTUREDROPDOWNMENU_H
