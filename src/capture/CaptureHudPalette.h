#pragma once
// ─────────────────────────────────────────────────────────────────────────────
// CaptureHudPalette.h — 截图与录屏全域双层语义微晶视觉令牌单一事实源 (SSOT)
//
// 设计哲学（Interactive Porcelain Studio vs High-Contrast Obsidian Telemetry）:
//   1. 可交互操作台（主/副工具栏、录屏控制栏、长截图控制栏、无级滑块、下拉菜单、尺寸胶囊）
//      统一采用温润雪瓷白微晶底板（Clean Porcelain White），干净高级、久看不厌；
//   2. 外围只读穿透遥测 HUD（左下角快捷键面板 ShortcutHintOverlay + 像素取色放大镜信息舱）
//      对标 PixPin 采用高通透曜石纯黑底舱 + 100% 纯白高亮实心键帽 + 纯白清晰说明文字，
//      既在任意明暗桌面上保持极高可读性，又与可点击的雪瓷白工具栏形成直觉视觉分层。
// ─────────────────────────────────────────────────────────────────────────────

#ifndef TOOLS3000_CAPTURE_HUD_PALETTE_H
#define TOOLS3000_CAPTURE_HUD_PALETTE_H

#include <d2d1.h>

namespace tools3000::capture::CaptureHudPalette {

    // --- 1. 可交互操作面板（工具栏 / 尺寸胶囊 / 菜单 / 滑块浮岛）温润雪瓷白基底 ---
    inline constexpr D2D1_COLOR_F kPanelBg             = { 0.992f, 0.994f, 0.998f, 0.97f }; // #FDFEFF 温润雪瓷白
    inline constexpr D2D1_COLOR_F kPanelOuterBorder    = { 0.059f, 0.090f, 0.165f, 0.13f }; // #0F172A 13% 细腻石板灰冷描边
    inline constexpr D2D1_COLOR_F kPanelInnerHighlight = { 1.000f, 1.000f, 1.000f, 0.92f }; // 顶部纯白微晶高光切线

    // 主工具栏左侧 2x3 六点拖拽手柄色值 (PixPin Benchmark Grip Dots)
    inline constexpr D2D1_COLOR_F kGripDotIdle         = { 0.392f, 0.455f, 0.545f, 0.56f }; // #64748B 优雅浅石板灰点阵
    inline constexpr D2D1_COLOR_F kGripDotHover        = { 0.118f, 0.161f, 0.231f, 0.88f }; // #1E293B 悬停/拖拽清晰加深

    // --- 2. 尺寸胶囊 HUD (drawSizeInfo & drawSizeUnitMenu) ---
    inline constexpr D2D1_COLOR_F kSizeHudBg           = { 0.992f, 0.994f, 0.998f, 0.96f };
    inline constexpr D2D1_COLOR_F kSizeHudHoverBg      = { 1.000f, 1.000f, 1.000f, 0.99f };
    inline constexpr D2D1_COLOR_F kSizeHudBorder       = { 0.059f, 0.090f, 0.165f, 0.14f };
    inline constexpr D2D1_COLOR_F kSizeHudHoverBorder  = { 0.059f, 0.090f, 0.165f, 0.26f };
    inline constexpr D2D1_COLOR_F kSizeHudDivider      = { 0.059f, 0.090f, 0.165f, 0.11f };

    // --- 3. 像素取色放大镜 (drawSelectionLoupe) 高对比曜石黑信息舱 + 清透蓝十字准星 ---
    inline constexpr D2D1_COLOR_F kLoupeInfoBg         = { 0.047f, 0.055f, 0.075f, 0.91f }; // #0C0E13 高对比曜石黑底舱
    inline constexpr D2D1_COLOR_F kLoupeCrosshairBand  = { 0.231f, 0.510f, 0.965f, 0.34f }; // #3B82F6 清透科技蓝十字准星带
    inline constexpr D2D1_COLOR_F kLoupePrimaryText    = { 1.000f, 1.000f, 1.000f, 0.98f }; // 纯白高亮色值文字
    inline constexpr D2D1_COLOR_F kLoupeSecondaryText  = { 0.910f, 0.930f, 0.965f, 0.92f }; // 明亮银白坐标与辅助说明
    inline constexpr D2D1_COLOR_F kLoupeKeycapBg       = { 1.000f, 1.000f, 1.000f, 0.96f }; // 纯白高亮实心微键帽
    inline constexpr D2D1_COLOR_F kLoupeKeycapText     = { 0.059f, 0.090f, 0.165f, 1.00f }; // 深曜石黑键帽字
    inline constexpr D2D1_COLOR_F kMintToastBg         = { 0.063f, 0.725f, 0.506f, 0.30f }; // 复制成功薄荷翡翠柔光底
    inline constexpr D2D1_COLOR_F kMintToastBorder     = { 0.133f, 0.827f, 0.596f, 0.68f };
    inline constexpr D2D1_COLOR_F kMintToastText       = { 0.720f, 0.980f, 0.855f, 1.00f }; // 明亮薄荷白字
    inline constexpr D2D1_COLOR_F kFooterHintBg        = { 1.000f, 1.000f, 1.000f, 0.10f }; // 曜石黑舱内微晶透底条
    inline constexpr D2D1_COLOR_F kFooterHintBorder    = { 1.000f, 1.000f, 1.000f, 0.16f };
    inline constexpr D2D1_COLOR_F kFooterHintText      = { 0.960f, 0.975f, 1.000f, 0.95f };

    // --- 4. 左下角快捷键指引 (ShortcutHintOverlay) 明亮清晰高对比体系 (PixPin Benchmark) ---
    inline constexpr D2D1_COLOR_F kShortcutPanelBg     = { 0.043f, 0.051f, 0.071f, 0.88f }; // #0B0D12 高通透纯正深黑底舱
    inline constexpr D2D1_COLOR_F kShortcutPanelBorder = { 1.000f, 1.000f, 1.000f, 0.14f }; // 极细微晶冷白边界
    inline constexpr D2D1_COLOR_F kShortcutKeyBg       = { 1.000f, 1.000f, 1.000f, 0.98f }; // #FFFFFF 100% 纯白高亮实心键帽
    inline constexpr D2D1_COLOR_F kShortcutKeyBorder   = { 1.000f, 1.000f, 1.000f, 1.00f };
    inline constexpr D2D1_COLOR_F kShortcutKeyText     = { 0.059f, 0.090f, 0.165f, 1.00f }; // #0F172A 纯黑锐利粗体键名
    inline constexpr D2D1_COLOR_F kShortcutLabelText   = { 1.000f, 1.000f, 1.000f, 0.96f }; // #FFFFFF 明亮清晰说明文字
    inline constexpr D2D1_COLOR_F kShortcutHeaderText  = { 1.000f, 1.000f, 1.000f, 0.98f };
    inline constexpr D2D1_COLOR_F kShortcutDotAccent   = { 1.000f, 1.000f, 1.000f, 0.92f };
    inline constexpr D2D1_COLOR_F kShortcutDivider     = { 1.000f, 1.000f, 1.000f, 0.12f };

    // --- 5. 雪瓷白操作台文本与统一无彩色深石板微点缀层级 ---
    inline constexpr D2D1_COLOR_F kPrimaryText         = { 0.059f, 0.090f, 0.165f, 0.98f }; // #0F172A 主文本（宽高数值/主标签）
    inline constexpr D2D1_COLOR_F kSecondaryText       = { 0.278f, 0.333f, 0.412f, 0.94f }; // #475569 次文本（X,Y 坐标/辅助说明）
    inline constexpr D2D1_COLOR_F kMutedText           = { 0.392f, 0.455f, 0.545f, 0.90f }; // #64748B 轻柔修饰符与箭头

    // 胶囊徽章与开关激活态（统一克制的高质感石板墨黑灰，零刺眼杂色）
    inline constexpr D2D1_COLOR_F kChipAccentBg        = { 0.059f, 0.090f, 0.165f, 0.08f }; // 浅冷灰胶囊底
    inline constexpr D2D1_COLOR_F kChipHoverBg         = { 0.059f, 0.090f, 0.165f, 0.13f };
    inline constexpr D2D1_COLOR_F kChipAccentBorder    = { 0.059f, 0.090f, 0.165f, 0.15f }; // 细腻石板灰边
    inline constexpr D2D1_COLOR_F kChipHoverBorder     = { 0.059f, 0.090f, 0.165f, 0.24f };
    inline constexpr D2D1_COLOR_F kChipAccentText      = { 0.118f, 0.161f, 0.231f, 0.96f }; // #1E293B 沉稳深石板字
    inline constexpr D2D1_COLOR_F kToggleOnBg          = { 0.118f, 0.161f, 0.231f, 0.95f }; // #1E293B 深石墨激活胶囊
    inline constexpr D2D1_COLOR_F kToggleOffBg         = { 0.796f, 0.835f, 0.882f, 0.92f }; // #CBD5E1 柔和云灰关闭轨道

    // --- 6. 选区边框与辅助导引线：全天候双层无色偏微晶光学框 ---
    inline constexpr D2D1_COLOR_F kSelectionOuterFrame = { 0.059f, 0.090f, 0.165f, 0.52f }; // 外层高反差深石板衬边
    inline constexpr D2D1_COLOR_F kSelectionInnerFrame = { 1.000f, 1.000f, 1.000f, 0.96f }; // 内层纯净白晶主线
    inline constexpr D2D1_COLOR_F kHandleFill          = { 1.000f, 1.000f, 1.000f, 1.00f }; // 纯白控制锚点圆心
    inline constexpr D2D1_COLOR_F kHandleBorder        = { 0.118f, 0.161f, 0.231f, 0.92f }; // 深石板锚点外环
    inline constexpr D2D1_COLOR_F kCornerGuideArrow    = { 0.118f, 0.161f, 0.231f, 0.90f }; // 圆角内拉参考弧色
    inline constexpr D2D1_COLOR_F kSmartAlignmentGuide = { 0.231f, 0.510f, 0.965f, 0.78f }; // 居中对齐辅助虚线

    // --- 7. 录屏与长截图主操作珊瑚绯红强调色（唯一语义警示/停止/完成色彩） ---
    inline constexpr D2D1_COLOR_F kRecordCoralPrimary  = { 0.937f, 0.267f, 0.267f, 0.98f }; // #EF4444 珊瑚绯红
    inline constexpr D2D1_COLOR_F kRecordCoralHover    = { 0.863f, 0.149f, 0.149f, 1.00f }; // #DC2626 悬停加深
    inline constexpr D2D1_COLOR_F kRecordCoralSoftBg   = { 0.937f, 0.267f, 0.267f, 0.12f }; // 呼吸灯柔光晕

} // namespace tools3000::capture::CaptureHudPalette

#endif // TOOLS3000_CAPTURE_HUD_PALETTE_H
