#pragma once
// ─────────────────────────────────────────────────────────────────────────────
// OcrEngine — OCR 引擎抽象
//
// 职责:
//   1. 封装 PaddleOCR Lite 等 OCR 引擎的调用逻辑
//   2. 支持通过外部进程通信（JSON）或内嵌库实现文字提取
// ─────────────────────────────────────────────────────────────────────────────

#ifndef TOOLS3000_OCR_OCRENGINE_H
#define TOOLS3000_OCR_OCRENGINE_H

#include <string>
#include <vector>
#include <opencv2/core.hpp>

namespace tools3000::ocr {

struct OcrResult {
    std::string text;
    float confidence = 0.0f;
    cv::Rect box;
};

struct OcrWordToken {
    std::wstring text;
    float x = 0.0f;
    float y = 0.0f;
    float w = 0.0f;
    float h = 0.0f;
    float cy() const noexcept { return y + h * 0.5f; }
    float right() const noexcept { return x + w; }
    float bottom() const noexcept { return y + h; }
};

class OcrEngine {
public:
    static OcrEngine& instance();

    /// 初始化引擎 (探测可用的 OCR 语言包)。
    bool initialize();

    /// 关闭引擎
    void shutdown();

    /// 系统是否存在可用的 OCR 语言包。
    bool isAvailable() const { return m_available; }

    /// 提取文字 (逐行结果 + 包围盒)。
    std::vector<OcrResult> extractText(const cv::Mat& image);

    /// 提取文字并按行拼接为单个字符串。
    std::string recognizeToText(const cv::Mat& image);

    /// 从磁盘图片文件识别文字 (UTF-8 路径)。失败返回空串。
    std::string recognizeImageFile(const std::string& utf8Path);

    /// 基于递归 XY-Cut (垂直空白投影分栏 + 水平跨栏切分) 将原始词块聚类为有序行结果，栏内大间距使用 \t 拼接
    static std::vector<OcrResult> assembleLinesWithXYCut(
        std::vector<OcrWordToken> tokens,
        float scaleFactor = 1.0f,
        int borderPad = 0,
        int maxImageWidth = 0,
        int maxImageHeight = 0);

    /// 对已识别文本块集合执行 XY-Cut 分栏阅读序重排与同行拼接（供贴图选词复制与全选复制复用）
    static std::string orderTextBlocksWithXYCut(
        const std::vector<OcrResult>& blocks,
        const char* lineEnding = "\r\n");

    /// 智能合并段落内因版面宽度产生的硬换行，保留真实段落/列表/代码块换行
    static std::wstring smartMergeParagraphs(const std::wstring& rawMultiLineText);
    static std::string smartMergeParagraphsUtf8(const std::string& rawMultiLineUtf8);

private:
    OcrEngine() = default;
    ~OcrEngine() = default;
    OcrEngine(const OcrEngine&) = delete;
    OcrEngine& operator=(const OcrEngine&) = delete;

    bool m_available = false;
};

} // namespace tools3000::ocr

#endif // TOOLS3000_OCR_OCRENGINE_H
