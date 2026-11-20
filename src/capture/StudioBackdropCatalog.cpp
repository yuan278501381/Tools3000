// ─────────────────────────────────────────────────────────────────────────────
// StudioBackdropCatalog.cpp — 演播室 49 款世界级艺术策展与大师配色单一事实源实现
// ─────────────────────────────────────────────────────────────────────────────

#include "capture/StudioBackdropCatalog.h"

#include <algorithm>
#include <cctype>

namespace tools3000::capture {
namespace {

inline cv::Vec3f rgb(uint8_t r, uint8_t g, uint8_t b) noexcept {
    return cv::Vec3f(static_cast<float>(b), static_cast<float>(g), static_cast<float>(r));
}

std::string toAsciiLower(const std::string& s) {
    std::string out = s;
    std::transform(out.begin(), out.end(), out.begin(), [](unsigned char ch) {
        return static_cast<char>(std::tolower(ch));
    });
    return out;
}

StudioMeshBackdropSpec makeSolidSpec(uint8_t r, uint8_t g, uint8_t b, bool isLight) noexcept {
    const cv::Vec3f base = rgb(r, g, b);
    const cv::Vec3f lift = isLight
        ? cv::Vec3f((std::min)(255.0f, base[0] + 7.0f), (std::min)(255.0f, base[1] + 7.0f), (std::min)(255.0f, base[2] + 7.0f))
        : cv::Vec3f((std::min)(255.0f, base[0] + 18.0f), (std::min)(255.0f, base[1] + 18.0f), (std::min)(255.0f, base[2] + 18.0f));
    const cv::Vec3f shade = cv::Vec3f(
        (std::max)(0.0f, base[0] - 10.0f),
        (std::max)(0.0f, base[1] - 10.0f),
        (std::max)(0.0f, base[2] - 10.0f));
    return {
        base,
        {{
            {0.06f, 0.06f, 0.85f, lift, 0.32f},
            {0.94f, 0.94f, 0.85f, shade, 0.26f},
            {0.94f, 0.08f, 0.65f, lift, 0.14f},
            {0.50f, 0.50f, 0.50f, base, 0.0f},
        }},
        isLight ? 0.02f : 0.06f,
        isLight ? 1.1f : 1.6f
    };
}

std::vector<StudioBackdropPreset> buildCatalog() {
    std::vector<StudioBackdropPreset> list;
    list.reserve(49);

    // =========================================================================
    // 第一部分：大师策展首屏旗舰艺术壁纸（21 张 100% 原创壁纸）
    // 首屏第 1~2 行置顶最具通透体积光、色彩高级感与边缘表现力的殿堂级主视觉
    // =========================================================================

    // 1. [旗舰主视觉] 蔚蓝流金丝浪 (Royal Azure & Sunlit Gold Silk)
    list.push_back({
        "blossom_silk", L"蔚蓝流金", L"Sunlit Azure", L"blossom_silk.jpg",
        BackdropCat_Light, false, false, BeautyBackgroundType::AiryGlacier,
        { rgb(18, 68, 178), {{
            {0.05f, 0.08f, 0.78f, rgb(16, 198, 224), 0.88f},
            {0.94f, 0.10f, 0.76f, rgb(254, 176, 108), 0.86f},
            {0.92f, 0.92f, 0.76f, rgb(32, 186, 218), 0.84f},
            {0.08f, 0.92f, 0.72f, rgb(248, 132, 98), 0.80f},
        }}, 0.04f, 1.6f }
    });
    // 2. [旗舰主视觉] 琉璃花语 (iPad Pro Dichroic Glass Petals)
    list.push_back({
        "crimson_velvet", L"琉璃花语", L"Dichroic Bloom", L"crimson_velvet.jpg",
        BackdropCat_Dark, false, false, BeautyBackgroundType::StudioSlate,
        { rgb(14, 12, 34), {{
            {0.05f, 0.08f, 0.72f, rgb(226, 98, 138), 0.86f},
            {0.95f, 0.08f, 0.70f, rgb(182, 86, 178), 0.82f},
            {0.94f, 0.92f, 0.74f, rgb(238, 146, 108), 0.86f},
            {0.08f, 0.94f, 0.64f, rgb(118, 62, 148), 0.74f},
        }}, 0.10f, 1.8f }
    });
    // 3. [旗舰主视觉] 暗夜流金棱镜 (Raycast Volumetric Gold Prism)
    list.push_back({
        "obsidian_gold", L"流金棱镜", L"Gold Caustics", L"obsidian_gold.jpg",
        BackdropCat_Dark, false, false, BeautyBackgroundType::MidnightGraphite,
        { rgb(8, 10, 18), {{
            {0.92f, 0.42f, 0.72f, rgb(236, 162, 64), 0.86f},
            {0.58f, 0.06f, 0.68f, rgb(146, 176, 232), 0.76f},
            {0.10f, 0.86f, 0.68f, rgb(198, 134, 52), 0.74f},
            {0.06f, 0.08f, 0.64f, rgb(18, 24, 42), 0.65f},
        }}, 0.12f, 2.0f }
    });
    // 4. [旗舰主视觉] 深海液态镜 (Sapphire Liquid Glass)
    list.push_back({
        "abyssal_indigo", L"深海液态镜", L"Sapphire Glass", L"abyssal_indigo.jpg",
        BackdropCat_Dark, false, false, BeautyBackgroundType::StudioSlate,
        { rgb(10, 20, 46), {{
            {0.05f, 0.08f, 0.76f, rgb(30, 96, 212), 0.84f},
            {0.95f, 0.88f, 0.74f, rgb(28, 178, 218), 0.76f},
            {0.92f, 0.10f, 0.62f, rgb(74, 122, 244), 0.60f},
            {0.08f, 0.92f, 0.60f, rgb(14, 52, 128), 0.62f},
        }}, 0.10f, 2.0f }
    });
    // 5. [旗舰主视觉] 全息霞光 (Hologram Liquid Mist)
    list.push_back({
        "lavender_mist", L"全息霞光", L"Hologram Mist", L"lavender_mist.jpg",
        BackdropCat_Light, true, false, BeautyBackgroundType::PureMinimal,
        { rgb(168, 178, 250), {{
            {0.94f, 0.06f, 0.78f, rgb(236, 72, 232), 0.88f},
            {0.05f, 0.92f, 0.78f, rgb(45, 224, 228), 0.88f},
            {0.06f, 0.08f, 0.66f, rgb(124, 118, 246), 0.74f},
            {0.94f, 0.92f, 0.70f, rgb(118, 166, 255), 0.82f},
        }}, 0.03f, 1.6f }
    });
    // 6. [旗舰主视觉] 云海圣光 (Golden Cloudscape Rays)
    list.push_back({
        "sunlit_cream", L"云海圣光", L"Golden Clouds", L"sunlit_cream.jpg",
        BackdropCat_Light | BackdropCat_Scene, true, false, BeautyBackgroundType::PaperChalk,
        { rgb(236, 214, 198), {{
            {0.06f, 0.08f, 0.78f, rgb(142, 198, 226), 0.86f},
            {0.94f, 0.12f, 0.76f, rgb(186, 220, 236), 0.82f},
            {0.92f, 0.90f, 0.78f, rgb(252, 206, 158), 0.86f},
            {0.08f, 0.92f, 0.74f, rgb(238, 192, 168), 0.82f},
        }}, 0.03f, 1.4f }
    });
    // 7. [旗舰主视觉] 暮雪镜湖 (Sakura Twilight Mirror Peak)
    list.push_back({
        "desert_dunes", L"暮雪镜湖", L"Twilight Peak", L"desert_dunes.jpg",
        BackdropCat_Scene, false, false, BeautyBackgroundType::DuskMountains,
        { rgb(92, 86, 138), {{
            {0.08f, 0.08f, 0.76f, rgb(86, 98, 162), 0.84f},
            {0.94f, 0.42f, 0.76f, rgb(252, 192, 178), 0.86f},
            {0.10f, 0.92f, 0.74f, rgb(52, 56, 92), 0.84f},
            {0.92f, 0.90f, 0.74f, rgb(196, 136, 128), 0.80f},
        }}, 0.06f, 1.6f }
    });

    // --- 第 2 行：高定材质与大气自然精选 ---
    // 8. 幻彩贝母 (Pearl Nacre Silk)
    list.push_back({
        "mint_porcelain", L"幻彩贝母", L"Pearl Nacre", L"mint_porcelain.jpg",
        BackdropCat_Light, true, false, BeautyBackgroundType::PureMinimal,
        { rgb(238, 236, 244), {{
            {0.06f, 0.10f, 0.78f, rgb(248, 214, 228), 0.84f},
            {0.94f, 0.12f, 0.76f, rgb(202, 242, 236), 0.84f},
            {0.08f, 0.92f, 0.74f, rgb(212, 238, 242), 0.82f},
            {0.92f, 0.90f, 0.74f, rgb(242, 222, 246), 0.82f},
        }}, 0.02f, 1.4f }
    });
    // 9. 赤霞暮岭
    list.push_back({
        "coastal_horizon", L"赤霞暮岭", L"Crimson Horizon", L"coastal_horizon.jpg",
        BackdropCat_Scene, false, false, BeautyBackgroundType::DuskMountains,
        { rgb(198, 52, 34), {{
            {0.08f, 0.08f, 0.78f, rgb(186, 36, 32), 0.84f},
            {0.92f, 0.12f, 0.76f, rgb(246, 96, 44), 0.86f},
            {0.50f, 0.64f, 0.70f, rgb(254, 152, 54), 0.78f},
            {0.50f, 0.96f, 0.74f, rgb(22, 26, 38), 0.92f},
        }}, 0.08f, 1.8f }
    });
    // 10. 翡翠暗光
    list.push_back({
        "carbon_aurora", L"翡翠暗光", L"Emerald Nocturne", L"carbon_aurora.jpg",
        BackdropCat_Dark, false, false, BeautyBackgroundType::SilkMist,
        { rgb(8, 14, 12), {{
            {0.04f, 0.06f, 0.76f, rgb(38, 204, 72), 0.86f},
            {0.94f, 0.10f, 0.68f, rgb(16, 172, 152), 0.74f},
            {0.08f, 0.92f, 0.66f, rgb(12, 92, 68), 0.62f},
            {0.94f, 0.94f, 0.58f, rgb(12, 22, 28), 0.55f},
        }}, 0.12f, 2.2f }
    });
    // 11. 极夜星岚
    list.push_back({
        "aurora_night", L"极夜星岚", L"Aurora Night", L"aurora_night.jpg",
        BackdropCat_Scene | BackdropCat_Dark, false, false, BeautyBackgroundType::StudioSlate,
        { rgb(12, 22, 44), {{
            {0.12f, 0.16f, 0.72f, rgb(44, 198, 152), 0.78f},
            {0.90f, 0.14f, 0.70f, rgb(52, 162, 192), 0.74f},
            {0.08f, 0.92f, 0.74f, rgb(16, 28, 54), 0.84f},
            {0.92f, 0.92f, 0.74f, rgb(20, 34, 62), 0.84f},
        }}, 0.10f, 1.8f }
    });
    // 12. 雪峰初阳
    list.push_back({
        "alpine_sunrise", L"雪峰初阳", L"Alpine Sunrise", L"alpine_sunrise.jpg",
        BackdropCat_Scene | BackdropCat_Light, true, false, BeautyBackgroundType::DuskMountains,
        { rgb(214, 220, 236), {{
            {0.90f, 0.24f, 0.72f, rgb(254, 206, 182), 0.84f},
            {0.10f, 0.10f, 0.76f, rgb(156, 192, 236), 0.82f},
            {0.10f, 0.92f, 0.70f, rgb(186, 178, 202), 0.70f},
            {0.90f, 0.90f, 0.70f, rgb(148, 166, 198), 0.72f},
        }}, 0.04f, 1.5f }
    });
    // 13. 暮色苍山
    list.push_back({
        "dusk_mountains", L"暮色苍山", L"Dusk Peaks", L"dusk_mountains.jpg",
        BackdropCat_Scene, false, true, BeautyBackgroundType::DuskMountains,
        { rgb(54, 48, 78), {{
            {0.84f, 0.34f, 0.72f, rgb(242, 160, 118), 0.82f},
            {0.10f, 0.10f, 0.74f, rgb(56, 72, 118), 0.80f},
            {0.10f, 0.92f, 0.72f, rgb(28, 34, 52), 0.84f},
            {0.92f, 0.90f, 0.68f, rgb(36, 40, 62), 0.80f},
        }}, 0.08f, 1.8f }
    });
    // 14. 拂晓湖山
    list.push_back({
        "misty_lake_dawn", L"拂晓湖山", L"Misty Lake", L"misty_lake_dawn.jpg",
        BackdropCat_Scene, false, true, BeautyBackgroundType::MistyLakeDawn,
        { rgb(125, 168, 198), {{
            {0.18f, 0.16f, 0.74f, rgb(242, 224, 198), 0.78f},
            {0.88f, 0.14f, 0.72f, rgb(146, 186, 218), 0.76f},
            {0.12f, 0.90f, 0.72f, rgb(58, 92, 116), 0.82f},
            {0.90f, 0.90f, 0.72f, rgb(66, 98, 118), 0.80f},
        }}, 0.07f, 1.6f }
    });

    // --- 第 3 行：北欧与极简丝绸经典 ---
    // 15. 北欧峡湾
    list.push_back({
        "nordic_fjord", L"北欧峡湾", L"Nordic Fjord", L"nordic_fjord.jpg",
        BackdropCat_Scene, false, false, BeautyBackgroundType::MistyLakeDawn,
        { rgb(84, 108, 126), {{
            {0.16f, 0.10f, 0.76f, rgb(192, 210, 224), 0.80f},
            {0.88f, 0.14f, 0.72f, rgb(174, 196, 214), 0.76f},
            {0.12f, 0.92f, 0.74f, rgb(42, 66, 86), 0.84f},
            {0.90f, 0.92f, 0.74f, rgb(38, 60, 78), 0.84f},
        }}, 0.08f, 1.6f }
    });
    // 16. 暮夜星云
    list.push_back({
        "studio_slate", L"暮夜星云", L"Velvet Dusk", L"studio_slate.jpg",
        BackdropCat_Dark, false, true, BeautyBackgroundType::StudioSlate,
        { rgb(22, 24, 44), {{
            {0.06f, 0.10f, 0.76f, rgb(68, 78, 152), 0.82f},
            {0.94f, 0.88f, 0.74f, rgb(104, 58, 132), 0.78f},
            {0.90f, 0.10f, 0.58f, rgb(88, 106, 168), 0.48f},
            {0.10f, 0.90f, 0.54f, rgb(48, 38, 84), 0.44f},
        }}, 0.10f, 2.0f }
    });
    // 17. 松烟墨翠
    list.push_back({
        "silk_mist", L"松烟墨翠", L"Pine & Emerald", L"silk_mist.jpg",
        BackdropCat_Dark, false, true, BeautyBackgroundType::SilkMist,
        { rgb(14, 30, 26), {{
            {0.06f, 0.10f, 0.74f, rgb(36, 98, 80), 0.80f},
            {0.94f, 0.88f, 0.72f, rgb(28, 72, 68), 0.72f},
            {0.90f, 0.10f, 0.58f, rgb(74, 136, 102), 0.46f},
            {0.10f, 0.90f, 0.56f, rgb(18, 46, 42), 0.44f},
        }}, 0.12f, 2.0f }
    });
    // 18. 深海曜石
    list.push_back({
        "midnight_graphite", L"深海曜石", L"Nordic Obsidian", L"midnight_graphite.jpg",
        BackdropCat_Dark, false, true, BeautyBackgroundType::MidnightGraphite,
        { rgb(14, 20, 30), {{
            {0.06f, 0.08f, 0.72f, rgb(42, 74, 112), 0.76f},
            {0.94f, 0.90f, 0.72f, rgb(56, 48, 86), 0.68f},
            {0.92f, 0.10f, 0.58f, rgb(64, 102, 138), 0.42f},
            {0.08f, 0.92f, 0.56f, rgb(20, 32, 48), 0.45f},
        }}, 0.12f, 2.0f }
    });
    // 19. 晨雾云母
    list.push_back({
        "pure_minimal", L"晨雾云母", L"Airy Cloud", L"pure_minimal.jpg",
        BackdropCat_Light, true, true, BeautyBackgroundType::PureMinimal,
        { rgb(241, 245, 250), {{
            {0.06f, 0.08f, 0.76f, rgb(206, 228, 255), 0.88f},
            {0.94f, 0.12f, 0.72f, rgb(252, 222, 242), 0.84f},
            {0.08f, 0.92f, 0.72f, rgb(222, 232, 255), 0.78f},
            {0.92f, 0.90f, 0.68f, rgb(246, 234, 254), 0.72f},
        }}, 0.02f, 1.4f }
    });
    // 20. 暖杏羊绒
    list.push_back({
        "paper_chalk", L"暖杏羊绒", L"Warm Cashmere", L"paper_chalk.jpg",
        BackdropCat_Light, true, true, BeautyBackgroundType::PaperChalk,
        { rgb(249, 243, 236), {{
            {0.06f, 0.10f, 0.78f, rgb(254, 226, 200), 0.86f},
            {0.94f, 0.88f, 0.74f, rgb(248, 210, 214), 0.80f},
            {0.92f, 0.10f, 0.64f, rgb(255, 244, 224), 0.74f},
            {0.08f, 0.90f, 0.62f, rgb(242, 224, 210), 0.66f},
        }}, 0.02f, 1.4f }
    });
    // 21. 晴空薄荷
    list.push_back({
        "airy_glacier", L"晴空薄荷", L"Glacier Mint", L"airy_glacier.jpg",
        BackdropCat_Light, true, true, BeautyBackgroundType::AiryGlacier,
        { rgb(240, 250, 248), {{
            {0.06f, 0.12f, 0.76f, rgb(198, 248, 238), 0.86f},
            {0.94f, 0.10f, 0.74f, rgb(202, 232, 255), 0.84f},
            {0.08f, 0.92f, 0.68f, rgb(224, 246, 242), 0.72f},
            {0.92f, 0.90f, 0.70f, rgb(214, 238, 252), 0.76f},
        }}, 0.02f, 1.4f }
    });

    // =========================================================================
    // 第二部分：28 款零体积流体丝绸极光着色器（Domain-Warped Fluid Silk）& 极简纯色
    // 经二维光滑旋流扭曲与丝绸折光调制，呈现 Apple Display P3 灵感大师色彩美学
    // =========================================================================

    // --- [第 1 行 · 高通透流光撞色系 · 7 款] ---
    list.push_back({
        "mesh_glacier_sky", L"霓虹紫青", L"Neon Prism", L"",
        BackdropCat_Mesh | BackdropCat_Light, true, false, BeautyBackgroundType::AiryGlacier,
        { rgb(108, 126, 248), {{
            {0.94f, 0.06f, 0.82f, rgb(244, 62, 232), 0.94f},
            {0.06f, 0.94f, 0.84f, rgb(24, 234, 236), 0.94f},
            {0.08f, 0.08f, 0.68f, rgb(76, 58, 178), 0.80f},
            {0.92f, 0.92f, 0.76f, rgb(86, 158, 255), 0.88f},
        }}, 0.03f, 2.0f }
    });
    list.push_back({
        "mesh_azure_wave", L"克莱因流光", L"Klein Lagoon", L"",
        BackdropCat_Mesh, false, false, BeautyBackgroundType::StudioSlate,
        { rgb(12, 46, 138), {{
            {0.05f, 0.08f, 0.82f, rgb(14, 138, 252), 0.94f},
            {0.95f, 0.90f, 0.82f, rgb(24, 228, 206), 0.90f},
            {0.92f, 0.08f, 0.72f, rgb(96, 186, 255), 0.82f},
            {0.08f, 0.92f, 0.70f, rgb(36, 72, 212), 0.84f},
        }}, 0.05f, 2.0f }
    });
    list.push_back({
        "mesh_indigo_pulse", L"星云蓝紫", L"Indigo Pulse", L"",
        BackdropCat_Mesh, false, false, BeautyBackgroundType::StudioSlate,
        { rgb(28, 20, 82), {{
            {0.05f, 0.06f, 0.82f, rgb(98, 72, 255), 0.94f},
            {0.95f, 0.08f, 0.78f, rgb(38, 208, 255), 0.88f},
            {0.94f, 0.94f, 0.82f, rgb(216, 70, 239), 0.90f},
            {0.06f, 0.94f, 0.72f, rgb(59, 130, 246), 0.82f},
        }}, 0.06f, 2.0f }
    });
    list.push_back({
        "mesh_aurora_mint", L"翡翠极光", L"Aurora Cyber", L"",
        BackdropCat_Mesh | BackdropCat_Dark, false, false, BeautyBackgroundType::SilkMist,
        { rgb(8, 24, 32), {{
            {0.05f, 0.06f, 0.84f, rgb(52, 218, 128), 0.92f},
            {0.94f, 0.10f, 0.78f, rgb(18, 192, 212), 0.86f},
            {0.08f, 0.94f, 0.72f, rgb(16, 118, 92), 0.76f},
            {0.94f, 0.94f, 0.76f, rgb(24, 36, 86), 0.86f},
        }}, 0.07f, 2.2f }
    });
    list.push_back({
        "mesh_breeze_aqua", L"马尔代夫", L"Maldives Mint", L"",
        BackdropCat_Mesh | BackdropCat_Light, true, false, BeautyBackgroundType::AiryGlacier,
        { rgb(212, 248, 246), {{
            {0.05f, 0.08f, 0.82f, rgb(72, 226, 206), 0.88f},
            {0.95f, 0.92f, 0.82f, rgb(96, 192, 254), 0.90f},
            {0.94f, 0.08f, 0.72f, rgb(254, 240, 198), 0.82f},
            {0.06f, 0.94f, 0.72f, rgb(156, 232, 254), 0.84f},
        }}, 0.02f, 1.6f }
    });
    list.push_back({
        "mesh_nordic_blue", L"莫奈睡莲", L"Monet Waterlily", L"",
        BackdropCat_Mesh | BackdropCat_Light, true, false, BeautyBackgroundType::PureMinimal,
        { rgb(206, 220, 244), {{
            {0.05f, 0.08f, 0.82f, rgb(138, 162, 228), 0.86f},
            {0.95f, 0.10f, 0.80f, rgb(168, 228, 222), 0.88f},
            {0.08f, 0.94f, 0.78f, rgb(198, 182, 242), 0.86f},
            {0.94f, 0.92f, 0.78f, rgb(232, 242, 255), 0.86f},
        }}, 0.02f, 1.6f }
    });
    list.push_back({
        "mesh_cobalt_bloom", L"深空电光蓝", L"Cobalt Bloom", L"",
        BackdropCat_Mesh, false, false, BeautyBackgroundType::StudioSlate,
        { rgb(16, 28, 78), {{
            {0.05f, 0.06f, 0.82f, rgb(37, 108, 255), 0.94f},
            {0.95f, 0.92f, 0.82f, rgb(146, 118, 255), 0.88f},
            {0.94f, 0.08f, 0.74f, rgb(56, 208, 255), 0.84f},
            {0.06f, 0.94f, 0.72f, rgb(28, 64, 198), 0.80f},
        }}, 0.06f, 2.0f }
    });

    // --- [第 2 行 · 落日霞光 & 莫兰迪高定系 · 7 款] ---
    list.push_back({
        "mesh_peach_blossom", L"京都晚樱", L"Kyoto Sakura", L"",
        BackdropCat_Mesh | BackdropCat_Light, true, false, BeautyBackgroundType::PaperChalk,
        { rgb(254, 226, 232), {{
            {0.05f, 0.08f, 0.82f, rgb(252, 126, 158), 0.88f},
            {0.95f, 0.92f, 0.82f, rgb(254, 178, 108), 0.90f},
            {0.94f, 0.08f, 0.74f, rgb(196, 182, 254), 0.82f},
            {0.06f, 0.94f, 0.74f, rgb(255, 214, 196), 0.84f},
        }}, 0.02f, 1.6f }
    });
    list.push_back({
        "mesh_sunset_glow", L"加州落日", L"Sunset Glow", L"",
        BackdropCat_Mesh, false, false, BeautyBackgroundType::PaperChalk,
        { rgb(92, 26, 54), {{
            {0.05f, 0.06f, 0.84f, rgb(255, 118, 36), 0.94f},
            {0.95f, 0.10f, 0.80f, rgb(244, 63, 148), 0.92f},
            {0.94f, 0.94f, 0.82f, rgb(98, 46, 224), 0.90f},
            {0.06f, 0.94f, 0.76f, rgb(251, 168, 56), 0.84f},
        }}, 0.05f, 2.0f }
    });
    list.push_back({
        "mesh_champagne_gold", L"香槟晨露", L"Champagne Silk", L"",
        BackdropCat_Mesh | BackdropCat_Light, true, false, BeautyBackgroundType::PaperChalk,
        { rgb(248, 236, 218), {{
            {0.05f, 0.06f, 0.82f, rgb(252, 204, 102), 0.86f},
            {0.95f, 0.92f, 0.82f, rgb(248, 168, 132), 0.86f},
            {0.94f, 0.08f, 0.74f, rgb(255, 244, 214), 0.84f},
            {0.06f, 0.94f, 0.74f, rgb(238, 196, 158), 0.82f},
        }}, 0.02f, 1.6f }
    });
    list.push_back({
        "mesh_rose_quartz", L"玫瑰粉晶", L"Rose Quartz", L"",
        BackdropCat_Mesh | BackdropCat_Light, true, false, BeautyBackgroundType::PureMinimal,
        { rgb(250, 228, 244), {{
            {0.05f, 0.08f, 0.82f, rgb(244, 118, 188), 0.88f},
            {0.95f, 0.92f, 0.82f, rgb(176, 138, 254), 0.90f},
            {0.94f, 0.08f, 0.74f, rgb(254, 186, 206), 0.84f},
            {0.06f, 0.94f, 0.74f, rgb(164, 214, 254), 0.82f},
        }}, 0.02f, 1.6f }
    });
    list.push_back({
        "mesh_amber_sand", L"托斯卡纳", L"Tuscan Amber", L"",
        BackdropCat_Mesh, false, false, BeautyBackgroundType::PaperChalk,
        { rgb(58, 22, 14), {{
            {0.05f, 0.06f, 0.82f, rgb(242, 138, 24), 0.92f},
            {0.95f, 0.92f, 0.80f, rgb(218, 68, 42), 0.88f},
            {0.94f, 0.08f, 0.74f, rgb(254, 198, 68), 0.80f},
            {0.06f, 0.94f, 0.72f, rgb(98, 36, 52), 0.82f},
        }}, 0.07f, 2.0f }
    });
    list.push_back({
        "mesh_lilac_cloud", L"普罗旺斯", L"Lilac Cloud", L"",
        BackdropCat_Mesh | BackdropCat_Light, true, false, BeautyBackgroundType::PureMinimal,
        { rgb(234, 228, 255), {{
            {0.05f, 0.06f, 0.82f, rgb(158, 136, 252), 0.90f},
            {0.95f, 0.08f, 0.80f, rgb(244, 162, 248), 0.88f},
            {0.06f, 0.94f, 0.80f, rgb(132, 198, 255), 0.88f},
            {0.94f, 0.94f, 0.76f, rgb(212, 188, 255), 0.84f},
        }}, 0.02f, 1.6f }
    });
    list.push_back({
        "mesh_velvet_berry", L"勃艮第星霞", L"Velvet Berry", L"",
        BackdropCat_Mesh, false, false, BeautyBackgroundType::StudioSlate,
        { rgb(56, 10, 32), {{
            {0.05f, 0.06f, 0.82f, rgb(228, 32, 92), 0.92f},
            {0.95f, 0.92f, 0.82f, rgb(142, 52, 244), 0.90f},
            {0.94f, 0.08f, 0.74f, rgb(252, 118, 132), 0.80f},
            {0.06f, 0.94f, 0.72f, rgb(86, 26, 128), 0.84f},
        }}, 0.07f, 2.0f }
    });

    // --- [第 3 行 · 奢华暗夜宝石流光 · 7 款] ---
    list.push_back({
        "mesh_dark_obsidian", L"黑曜蓝宝", L"Obsidian Sapphire", L"",
        BackdropCat_Mesh | BackdropCat_Dark, false, false, BeautyBackgroundType::MidnightGraphite,
        { rgb(8, 12, 24), {{
            {0.05f, 0.06f, 0.80f, rgb(32, 98, 186), 0.88f},
            {0.95f, 0.92f, 0.80f, rgb(72, 62, 188), 0.86f},
            {0.94f, 0.08f, 0.68f, rgb(44, 148, 198), 0.74f},
            {0.06f, 0.94f, 0.68f, rgb(18, 34, 72), 0.76f},
        }}, 0.09f, 2.2f }
    });
    list.push_back({
        "mesh_dark_violet", L"午夜紫晶", L"Midnight Amethyst", L"",
        BackdropCat_Mesh | BackdropCat_Dark, false, false, BeautyBackgroundType::StudioSlate,
        { rgb(12, 8, 28), {{
            {0.05f, 0.06f, 0.80f, rgb(118, 46, 218), 0.90f},
            {0.95f, 0.92f, 0.80f, rgb(188, 36, 108), 0.86f},
            {0.94f, 0.08f, 0.70f, rgb(66, 78, 196), 0.76f},
            {0.06f, 0.94f, 0.70f, rgb(42, 20, 92), 0.76f},
        }}, 0.09f, 2.2f }
    });
    list.push_back({
        "mesh_dark_emerald", L"暗夜祖母绿", L"Imperial Emerald", L"",
        BackdropCat_Mesh | BackdropCat_Dark, false, false, BeautyBackgroundType::SilkMist,
        { rgb(6, 20, 18), {{
            {0.05f, 0.06f, 0.80f, rgb(16, 158, 112), 0.90f},
            {0.95f, 0.92f, 0.80f, rgb(22, 142, 178), 0.86f},
            {0.94f, 0.08f, 0.68f, rgb(46, 178, 128), 0.74f},
            {0.06f, 0.94f, 0.68f, rgb(10, 48, 46), 0.76f},
        }}, 0.09f, 2.2f }
    });
    list.push_back({
        "mesh_dark_titanium", L"深空钛银", L"Space Titanium", L"",
        BackdropCat_Mesh | BackdropCat_Dark, false, false, BeautyBackgroundType::MidnightGraphite,
        { rgb(14, 18, 26), {{
            {0.05f, 0.06f, 0.82f, rgb(88, 102, 126), 0.84f},
            {0.95f, 0.92f, 0.80f, rgb(108, 118, 138), 0.80f},
            {0.94f, 0.08f, 0.68f, rgb(54, 68, 92), 0.72f},
            {0.06f, 0.94f, 0.68f, rgb(28, 36, 50), 0.76f},
        }}, 0.09f, 2.4f }
    });
    list.push_back({
        "mesh_dark_amber", L"黑金流沙", L"Nocturne Gold", L"",
        BackdropCat_Mesh | BackdropCat_Dark, false, false, BeautyBackgroundType::MidnightGraphite,
        { rgb(16, 12, 8), {{
            {0.05f, 0.06f, 0.80f, rgb(192, 112, 32), 0.88f},
            {0.95f, 0.92f, 0.80f, rgb(214, 146, 48), 0.84f},
            {0.94f, 0.08f, 0.68f, rgb(118, 68, 24), 0.74f},
            {0.06f, 0.94f, 0.68f, rgb(38, 26, 16), 0.76f},
        }}, 0.10f, 2.2f }
    });
    list.push_back({
        "mesh_dark_crimson", L"暗夜红宝", L"Ruby Nocturne", L"",
        BackdropCat_Mesh | BackdropCat_Dark, false, false, BeautyBackgroundType::MidnightGraphite,
        { rgb(20, 8, 16), {{
            {0.05f, 0.06f, 0.80f, rgb(198, 28, 82), 0.90f},
            {0.95f, 0.92f, 0.80f, rgb(132, 42, 186), 0.84f},
            {0.94f, 0.08f, 0.68f, rgb(146, 36, 72), 0.74f},
            {0.06f, 0.94f, 0.68f, rgb(32, 16, 44), 0.76f},
        }}, 0.10f, 2.2f }
    });
    list.push_back({
        "mesh_silver_mist", L"铂金云雾", L"Platinum Silk", L"",
        BackdropCat_Mesh | BackdropCat_Light, true, false, BeautyBackgroundType::PureMinimal,
        { rgb(228, 236, 246), {{
            {0.05f, 0.06f, 0.82f, rgb(255, 255, 255), 0.90f},
            {0.95f, 0.92f, 0.80f, rgb(176, 194, 218), 0.86f},
            {0.94f, 0.08f, 0.72f, rgb(212, 228, 248), 0.82f},
            {0.06f, 0.94f, 0.72f, rgb(198, 212, 232), 0.80f},
        }}, 0.02f, 1.6f }
    });

    // --- [第 4 行 · 极简高定纯色 · 7 款] ---
    list.push_back({
        "solid_snow_white", L"极简雪白", L"Snow White", L"",
        BackdropCat_Mesh | BackdropCat_Light, true, false, BeautyBackgroundType::PureMinimal,
        makeSolidSpec(248, 250, 252, true)
    });
    list.push_back({
        "solid_warm_linen", L"暖灰羊皮", L"Warm Linen", L"",
        BackdropCat_Mesh | BackdropCat_Light, true, false, BeautyBackgroundType::PaperChalk,
        makeSolidSpec(244, 238, 230, true)
    });
    list.push_back({
        "solid_cool_slate", L"冷雾石板", L"Cool Slate", L"",
        BackdropCat_Mesh | BackdropCat_Light, true, false, BeautyBackgroundType::PureMinimal,
        makeSolidSpec(218, 226, 236, true)
    });
    list.push_back({
        "solid_apple_blue", L"经典晴蓝", L"Studio Blue", L"",
        BackdropCat_Mesh, false, false, BeautyBackgroundType::StudioSlate,
        makeSolidSpec(37, 99, 235, false)
    });
    list.push_back({
        "solid_sage_gray", L"莫兰迪青灰", L"Sage Slate", L"",
        BackdropCat_Mesh | BackdropCat_Dark, false, false, BeautyBackgroundType::SilkMist,
        makeSolidSpec(68, 84, 104, false)
    });
    list.push_back({
        "solid_graphite", L"深邃石墨", L"Deep Graphite", L"",
        BackdropCat_Mesh | BackdropCat_Dark, false, false, BeautyBackgroundType::MidnightGraphite,
        makeSolidSpec(28, 39, 56, false)
    });
    list.push_back({
        "solid_carbon_black", L"纯粹碳黑", L"Carbon Black", L"",
        BackdropCat_Mesh | BackdropCat_Dark, false, false, BeautyBackgroundType::MidnightGraphite,
        makeSolidSpec(11, 15, 23, false)
    });

    return list;
}

} // namespace

const std::vector<StudioBackdropPreset>& StudioBackdropCatalog::presets() {
    static const std::vector<StudioBackdropPreset> kCatalog = buildCatalog();
    return kCatalog;
}

std::string StudioBackdropCatalog::normalizePresetKey(const std::string& rawKey) {
    const std::string low = toAsciiLower(rawKey);
    if (low == "aurora_purple") return "studio_slate";
    if (low == "sunrise_coral") return "paper_chalk";
    if (low == "emerald_sea")   return "silk_mist";
    if (low == "obsidian_dark") return "midnight_graphite";
    if (low == "minimal_light") return "pure_minimal";
    return low;
}

const char* StudioBackdropCatalog::canonicalKeyForLegacyTheme(BeautyBackgroundType theme) noexcept {
    switch (theme) {
        case BeautyBackgroundType::MidnightGraphite: return "midnight_graphite";
        case BeautyBackgroundType::SilkMist:         return "silk_mist";
        case BeautyBackgroundType::PureMinimal:      return "pure_minimal";
        case BeautyBackgroundType::PaperChalk:       return "paper_chalk";
        case BeautyBackgroundType::AiryGlacier:      return "airy_glacier";
        case BeautyBackgroundType::DuskMountains:    return "dusk_mountains";
        case BeautyBackgroundType::MistyLakeDawn:    return "misty_lake_dawn";
        case BeautyBackgroundType::StudioSlate:
        default:                                     return "studio_slate";
    }
}

const StudioBackdropPreset* StudioBackdropCatalog::findByKey(const std::string& key) {
    if (key.empty()) return nullptr;
    const std::string norm = normalizePresetKey(key);
    for (const auto& preset : presets()) {
        if (norm == preset.key) {
            return &preset;
        }
    }
    return nullptr;
}

const StudioBackdropPreset& StudioBackdropCatalog::resolvePreset(
    const std::string& presetKey,
    BeautyBackgroundType legacyTheme) {
    if (const auto* hit = findByKey(presetKey)) {
        return *hit;
    }
    const char* legacyKey = canonicalKeyForLegacyTheme(legacyTheme);
    if (const auto* hit = findByKey(legacyKey)) {
        return *hit;
    }
    return presets().front();
}

bool StudioBackdropCatalog::matchesCategoryTab(const StudioBackdropPreset& preset, int categoryTab) noexcept {
    switch (categoryTab) {
        case 0: return true; // 全部
        case 1: return (preset.categoryMask & BackdropCat_Dark) != 0u;
        case 2: return (preset.categoryMask & BackdropCat_Light) != 0u;
        case 3: return (preset.categoryMask & BackdropCat_Scene) != 0u;
        case 4: return (preset.categoryMask & BackdropCat_Mesh) != 0u;
        default: return false;
    }
}

} // namespace tools3000::capture
