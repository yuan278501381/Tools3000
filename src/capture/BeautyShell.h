#pragma once
// ─────────────────────────────────────────────────────────────────────────────
// BeautyShell.h — 截图美化外壳导出引擎 (CleanShot X / PixPin 风格)
//
// 职责:
//   1. 自动为截图添加优雅可调节衬底画布 Padding
//   2. 提供多款专业预设渐变/纯色/微晶毛玻璃背景 (极光紫蓝/晨曦珊瑚/翡翠深海/曜石黑晶/极简浅灰)
//   3. 为内部截图施加平滑抗锯齿圆角与多层柔和高斯弥散投影
//   4. 叠加高质感微晶高光内边框，生成世界级分享图片
// ─────────────────────────────────────────────────────────────────────────────

#ifndef TOOLS3000_CAPTURE_BEAUTYSHELL_H
#define TOOLS3000_CAPTURE_BEAUTYSHELL_H

#include <opencv2/core.hpp>

namespace tools3000::capture {

/// 美化外壳背景预设类型
enum class BeautyBackgroundType {
    StudioSlate      = 0, // 极光星云 (Aurora Nebula: Indigo #6366F1 -> Violet #8B5CF6 -> Rose #EC4899)
    PaperChalk       = 1, // 晨曦落日 (Sunset Glow: Coral #FB7185 -> Amber #F97316 -> Gold #FACC15)
    SilkMist         = 2, // 碧海极光 (Oceanic Teal: Sky #0EA5E9 -> Teal #14B8A6 -> Emerald #10B981)
    MidnightGraphite = 3, // 曜石黑晶 (Obsidian Pro: Slate #334155 -> Obsidian #1E293B -> Carbon #090D16)
    PureMinimal      = 4, // 钛金画廊 (Titanium Studio: Alabaster #F8FAFC -> Silver #E2E8F0 -> Titanium #CBD5E1)
    COUNT,
    // 兼容历史命名别名
    AuroraPurple = StudioSlate,
    SunriseCoral = PaperChalk,
    EmeraldSea   = SilkMist,
    ObsidianDark = MidnightGraphite,
    MinimalLight = PureMinimal
};

/// 美化外壳配置参数
struct BeautyShellOptions {
    bool enabled = false;
    int padding = 48;                         // 衬底画布 Padding (px, 24~80)
    float cornerRadius = 16.0f;               // 截图圆角半径 (px)
    float shadowRadius = 28.0f;               // 柔和外阴影扩散半径 (px)
    float shadowOpacity = 0.45f;              // 阴影不透明度 (0.0~1.0)
    int shadowOffsetY = 12;                   // 阴影垂直自然下坠偏移 (px)
    BeautyBackgroundType bgType = BeautyBackgroundType::StudioSlate;
};

/// 对输入截图应用精美外壳合成
cv::Mat applyBeautyShell(const cv::Mat& src, const BeautyShellOptions& options);

} // namespace tools3000::capture

#endif // TOOLS3000_CAPTURE_BEAUTYSHELL_H
