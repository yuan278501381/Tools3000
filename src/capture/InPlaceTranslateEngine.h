#pragma once
// ─────────────────────────────────────────────────────────────────────────────
// InPlaceTranslateEngine.h — 微信级截图识别原位翻译排版与无痕合成引擎
//
// 职责:
//   1. 复用离线 OCR 版面聚类结果 (layoutRegionId / alignedRowId)，将碎片文字行智能聚合为
//      自然翻译段落（自动跳过纯数字/URL/纯符号噪声，严格避免跨表格列粘连）；
//   2. 复用 TextReplaceEngine 6 维视觉 DNA 与 LinearBgPlane / MI-GAN 512 / Telea 擦除原文字；
//   3. 基于 DirectWrite (IDWriteTextLayout) 执行多行自动换行、避障安全域计算与二分自适应字号排版，
//      生成非破坏性原位译图（绝不改动原图 m_cleanBaseImage），支持 0ms 原文/译文瞬切。
// ─────────────────────────────────────────────────────────────────────────────

#ifndef TOOLS3000_CAPTURE_INPLACETRANSLATEENGINE_H
#define TOOLS3000_CAPTURE_INPLACETRANSLATEENGINE_H

#include "capture/PinOcrTypes.h"
#include "capture/TextReplaceEngine.h"
#include "core/translate/TranslationTypes.h"

#include <opencv2/core.hpp>
#include <string>
#include <string_view>
#include <vector>

namespace tools3000::capture {

/// 聚合后的单个原位翻译段落模型
struct TranslationParagraph {
    cv::Rect unionRect;               // 段落整体包络框
    std::vector<cv::Rect> lineRects;  // 原始逐行文字框（供精准擦底，不误伤外围边框图标）
    std::vector<cv::Rect> charBoxes;  // 字符框集合（供 TextReplaceEngine 提取视觉字号与字重）
    int layoutRegionId = 0;
    int alignedRowId = -1;
    bool isSingleLine = true;
    std::string originalText;
    std::string translatedText;
    cv::Rect renderedRect;            // 实际渲染覆盖矩形
};

/// 完整原位译图合成结果
struct InPlaceTranslateRenderResult {
    bool success = false;
    cv::Mat translatedCanvas;
    std::vector<TranslationParagraph> paragraphs;
    size_t renderedCount = 0;
};

class InPlaceTranslateEngine {
public:
    /// 判断文本块是否值得送去翻译（自动跳过纯数字、纯标点、纯 URL/路径等噪声）
    static bool isTranslatableText(std::string_view utf8Text) noexcept;

    /// 将 OCR 原子文字块按版面结构聚类为自然翻译段落（支持全图或仅局部选中索引子集；
    /// 传入 sourceImage 时同步启用 0.05ms 视觉分隔墙检测，防止跨背景色/搜索框边框/灰黑层级误合并）
    static std::vector<TranslationParagraph> buildTranslationParagraphs(
        const std::vector<PinOcrBlock>& blocks,
        const cv::Size& imageSize,
        const std::vector<size_t>& selectedBlockIndices = {},
        const cv::Mat& sourceImage = cv::Mat());

    /// 合并全部已译段落文本（供一键「复制全部译文」使用）
    static std::string joinTranslatedTexts(const std::vector<TranslationParagraph>& paragraphs);

    /// 将已完成翻译的段落无损合成到原图副本上，返回全新原位译图（100% 不修改 cleanCanvas）
    /// extraObstacleRects 可传入所有未参与翻译但保留原图的 OCR 框与二维码框，确保译文绝不踩踏相邻编码、数字或二维码。
    static InPlaceTranslateRenderResult renderTranslatedCanvas(
        const cv::Mat& cleanCanvas,
        std::vector<TranslationParagraph> paragraphs,
        std::string_view targetLangCode = "zh-CN",
        const std::vector<cv::Rect>& extraObstacleRects = {});
};

} // namespace tools3000::capture

#endif // TOOLS3000_CAPTURE_INPLACETRANSLATEENGINE_H
