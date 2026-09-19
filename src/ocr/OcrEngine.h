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

private:
    OcrEngine() = default;
    ~OcrEngine() = default;
    OcrEngine(const OcrEngine&) = delete;
    OcrEngine& operator=(const OcrEngine&) = delete;

    bool m_available = false;
};

} // namespace tools3000::ocr

#endif // TOOLS3000_OCR_OCRENGINE_H
