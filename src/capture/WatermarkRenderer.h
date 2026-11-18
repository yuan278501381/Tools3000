#pragma once
// ─────────────────────────────────────────────────────────────────────────────
// WatermarkRenderer.h — 企业泄密溯源平铺水印与品牌图片/文字角标统一合成引擎 (SSOT)
//
// 版权声明: Copyright (c) 2026 Yy1 (yuan278501381) & Tools3000 contributors. MIT License.
// ─────────────────────────────────────────────────────────────────────────────

#ifndef TOOLS3000_CAPTURE_WATERMARKRENDERER_H
#define TOOLS3000_CAPTURE_WATERMARKRENDERER_H

#include <opencv2/core.hpp>

#include <string>
#include <string_view>
#include <vector>

namespace tools3000::capture {

enum class WatermarkMode : int {
    ImageLogo         = 0, // 透明通道图片 Logo 角标 (PNG / WebP / JPG / BMP)
    CornerText        = 1, // 单点品牌/版权文字角标
    TiledTraceability = 2, // 全屏斜向等距平铺企业合规溯源水印 (逐像素黑白极性自适应显影)
};

enum class WatermarkAnchor : int {
    TopLeft     = 0,
    TopRight    = 1,
    BottomLeft  = 2,
    BottomRight = 3,
    Center      = 4,
    CustomFree  = 5, // 舞台自由拖拽归一化中心坐标 (customNormX, customNormY)
};

struct WatermarkConfig {
    bool enabled = false;
    WatermarkMode mode = WatermarkMode::TiledTraceability;
    std::string imagePathUtf8;
    std::string textTemplate = "{user} · {host} · {date} {time}";
    WatermarkAnchor anchor = WatermarkAnchor::BottomRight;
    float customNormX = 0.88f; // [0.02 .. 0.98]
    float customNormY = 0.90f; // [0.02 .. 0.98]
    float scale = 1.00f;       // [0.35 .. 2.80], 1.0 = 100% 标准比例
    float opacity = 0.16f;     // [0.04 .. 1.00]
    float marginRatio = 0.026f;
    bool policyForced = false; // 由 HKLM\Software\Policies\Tools3000 强制开启并锁定
};

class WatermarkRenderer {
public:
    /// 解析动态环境占位符: {user}, {host}, {ip}, {date}, {time}, {datetime}
    static std::wstring resolveDynamicText(std::string_view utf8Template);

    /// 加载并缓存本地图片为 4 通道非预乘 BGRA（支持中文路径与 PNG/WebP 透明通道）
    static cv::Mat loadCachedLogoBgra(const std::string& imagePathUtf8);

    /// 在 BGR (CV_8UC3) 或 BGRA (CV_8UC4) 图像上原位合成水印；
    /// 若为单点角标模式 (ImageLogo / CornerText) 且实际绘制成功，通过 outBadgeBoundsPx 返回像素包围盒供舞台拖拽命中测试
    static bool applyInPlace(
        cv::Mat& targetBgrOrBgra,
        const WatermarkConfig& config,
        cv::Rect* outBadgeBoundsPx = nullptr);

    /// 计算单点角标在指定画布尺寸下的像素包围盒（不执行像素混合，供快速命中测试）
    static bool computeSingleBadgeBounds(
        int canvasWidth,
        int canvasHeight,
        const WatermarkConfig& config,
        cv::Rect& outBoundsPx);

    /// 读取全局静态截图企业/品牌水印配置（自动叠加 HKLM 组策略强制锁）
    static WatermarkConfig loadGlobalCaptureConfig();

    /// 若全局静态截图水印已开启（或受企业策略强制开启），返回压印水印后的新图像副本；否则零拷贝原样返回输入图像
    static cv::Mat applyGlobalCaptureWatermarkIfEnabled(const cv::Mat& cleanSource);

    /// 读取/保存录屏演播室水印配置与常用 Logo 历史路径列表
    static WatermarkConfig loadStudioWatermarkConfig();
    static void saveStudioWatermarkConfig(const WatermarkConfig& config);
    static std::vector<std::string> loadStudioRecentLogoPaths();
    static void pushStudioRecentLogoPath(const std::string& imagePathUtf8);
};

} // namespace tools3000::capture

#endif // TOOLS3000_CAPTURE_WATERMARKRENDERER_H
