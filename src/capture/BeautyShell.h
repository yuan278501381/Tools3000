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

/// 美化外壳与演播室背景预设类型（涵盖 沉稳 / 轻盈 / 风景 三大高定系列）
enum class BeautyBackgroundType {
    StudioSlate      = 0, // [沉稳] 暮夜星云 (Velvet Dusk: 深靛丝绒弥散网格)
    PaperChalk       = 1, // [轻盈] 暖杏羊绒 (Warm Cashmere: 北欧米杏与蜜桃柔光)
    SilkMist         = 2, // [沉稳] 松烟墨翠 (Pine & Emerald: 深林冷杉暗调微光)
    MidnightGraphite = 3, // [沉稳] 深海曜石 (Nordic Obsidian: 深海蓝炭黑钛微光)
    PureMinimal      = 4, // [轻盈] 晨雾云母 (Airy Cloud: 珍珠白与冰蓝樱粉丝绸网格)
    AiryGlacier      = 5, // [轻盈] 晴空薄荷 (Glacier Mint: 通透冰川青白弥散柔光)
    DuskMountains    = 6, // [风景] 暮色苍山 (Dusk Peaks: 落日晚霞与四层远山剪影柔雾)
    MistyLakeDawn    = 7, // [风景] 拂晓湖山 (Misty Lake: 破晓天青群山与静谧湖面微光)
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
