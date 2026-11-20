#pragma once
// ─────────────────────────────────────────────────────────────────────────────
// StudioBackdropCatalog.h — 演播室高定背景目录单一事实源 (SSOT)
//
// 职责:
//   1. 统一定义 21 款原创高清壁纸（沉稳 7 / 轻盈 7 / 风景 7）+ 28 款零体积
//      高定多光源高斯弥散渐变与极简纯色（共 49 款开箱即用背景）
//   2. 提供历史配置键别名兼容映射与分类过滤位掩码
//   3. 供 StudioVideoCompositor、StudioPreviewWindow、PluginEntry 共享零冗余复用
// ─────────────────────────────────────────────────────────────────────────────

#ifndef TOOLS3000_CAPTURE_STUDIOBACKDROPCATALOG_H
#define TOOLS3000_CAPTURE_STUDIOBACKDROPCATALOG_H

#include "capture/BeautyShell.h"

#include <opencv2/core.hpp>

#include <array>
#include <cstdint>
#include <string>
#include <vector>

namespace tools3000::capture {

struct StudioMeshLightOrb {
    float nx = 0.5f;
    float ny = 0.5f;
    float radius = 0.65f;
    cv::Vec3f bgr{0.0f, 0.0f, 0.0f};
    float weight = 0.0f;
};

struct StudioMeshBackdropSpec {
    cv::Vec3f baseBgr{24.0f, 27.0f, 46.0f};
    std::array<StudioMeshLightOrb, 4> orbs{}; // 四角边缘锚定光源球（Edge-First，套录屏外框依然色彩饱满）
    float vignetteStrength = 0.08f;
    float grainStrength = 2.0f;               // 确定性 TPDF 光学磨砂微颗粒强度（消色带 + 苹果官网级磨砂肌理）
};

/// 演播室背景分类位掩码（支持多标签，例如暗色渐变同时归属「沉稳」与「渐变」）
enum StudioBackdropCategoryMask : uint32_t {
    BackdropCat_None  = 0u,
    BackdropCat_Dark  = 1u << 0, // 沉稳
    BackdropCat_Light = 1u << 1, // 轻盈
    BackdropCat_Scene = 1u << 2, // 风景
    BackdropCat_Mesh  = 1u << 3, // 渐变 / 纯色
};

/// 演播室内置高定背景预设元数据（单一事实源）
struct StudioBackdropPreset {
    const char* key = "";                    // 持久化配置键（如 "midnight_graphite", "obsidian_gold", "mesh_glacier_sky"）
    const wchar_t* nameZh = L"";             // 简体中文名
    const wchar_t* nameEn = L"";             // 英文名
    const wchar_t* wallpaperFilename = L"";  // resources/wallpapers/<filename>，为空表示纯算法程序化渲染（0 KB 体积）
    uint32_t categoryMask = BackdropCat_None;
    bool isLightTheme = false;               // 浅色背景自动采用轻盈投影与高对比玻璃外边框
    bool hasLegacyEnum = false;
    BeautyBackgroundType legacyTheme = BeautyBackgroundType::StudioSlate;
    StudioMeshBackdropSpec meshSpec{};
};

class StudioBackdropCatalog {
public:
    /// 全部内置预设列表（21 张原创高清壁纸 + 28 款零体积多光源渐变/纯色 = 49 款）
    static const std::vector<StudioBackdropPreset>& presets();

    /// 将历史旧配置键（如 "aurora_purple"、"minimal_light"）规范化为当前标准键
    static std::string normalizePresetKey(const std::string& rawKey);

    /// 根据旧版枚举返回其对应的标准预设键
    static const char* canonicalKeyForLegacyTheme(BeautyBackgroundType theme) noexcept;

    /// 按配置键查找内置预设（支持历史别名）；找不到返回 nullptr
    static const StudioBackdropPreset* findByKey(const std::string& key);

    /// 解析当前生效预设：优先 presetKey，若为空或无效则回退 legacyTheme
    static const StudioBackdropPreset& resolvePreset(
        const std::string& presetKey,
        BeautyBackgroundType legacyTheme);

    /// 返回预设是否归属指定分类 Tab（0=全部, 1=沉稳, 2=轻盈, 3=风景, 4=渐变）
    static bool matchesCategoryTab(const StudioBackdropPreset& preset, int categoryTab) noexcept;
};

} // namespace tools3000::capture

#endif // TOOLS3000_CAPTURE_STUDIOBACKDROPCATALOG_H
