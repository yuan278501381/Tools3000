#pragma once
// ─────────────────────────────────────────────────────────────────────────────
// OcrEngine — 双引擎高精度离线文字识别与二维码检测中枢
//
// 职责:
//   1. 统一调度 PP-OCRv4 (OpenCV DNN ONNX) 与增强版 Windows.Media.Ocr 双离线引擎
//   2. 输出带 UTF-32 字符级精确子包围盒 (charBoxes) 的有序识别结果
//   3. 集成深底浅字/低对比毛玻璃 CLAHE + 反相自适应增强与 OpenCV quirc 二维码识别
//   4. 提供递归 XY-Cut 分栏阅读序聚类与段落智能合并能力
//
// 版权声明: Copyright (c) 2026 Yy1 (yuan278501381). All rights reserved.
// ─────────────────────────────────────────────────────────────────────────────

#ifndef TOOLS3000_OCR_OCRENGINE_H
#define TOOLS3000_OCR_OCRENGINE_H

#include <array>
#include <cstddef>
#include <opencv2/core.hpp>
#include <string>
#include <string_view>
#include <vector>

namespace tools3000::ocr {

struct OcrResult {
    std::string text;
    float confidence = 0.0f;
    cv::Rect box;
    /// 与 text 解码出的 UTF-32 Unicode 码点序列 1:1 对应的原图物理像素子包围盒
    std::vector<cv::Rect> charBoxes;
    /// 所属版面分栏/局部 UI 区块 ID（同区域内流式阅读，跨区域隔离防串行）
    int layoutRegionId = 0;
    /// 严格水平对齐的逻辑行 ID（同区域且基线严格对齐时共享同一 ID，用于整行悬停高亮）
    int alignedRowId = -1;
};

struct OcrQrResult {
    std::string text;
    cv::Rect boundingBox;
    std::array<cv::Point2f, 4> corners{};
    /// 条码/二维码制式标识（如 "QR_CODE"、"EAN_13"、"EAN_8"、"UPC_A"、"UPC_E"）
    std::string format = "QR_CODE";
};

struct OcrPageExtraction {
    std::vector<OcrResult> lines;
    std::vector<OcrQrResult> qrCodes;
    std::string activeEngine; // "ppocr_v4" | "windows"
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

    /// 初始化引擎 (探测可用的 Windows OCR 语言包与本地 PP-OCRv4 模型包)。
    bool initialize();

    /// 关闭引擎并释放缓存资源
    void shutdown();

    /// 系统是否存在可用的离线 OCR 引擎 (Windows 语言包或已安装 PP-OCRv4 模型包)。
    bool isAvailable() const;

    /// Windows.Media.Ocr 语言包是否就绪
    bool isWindowsOcrAvailable() const noexcept { return m_available; }

    /// 提取文字 (逐行结果 + 行包围盒 + 字符级精确 charBoxes)。
    std::vector<OcrResult> extractText(const cv::Mat& image);

    /// 完整提取页面实景信息 (逐行文字 + charBoxes + 图中二维码 + 实际生效引擎标识)。
    OcrPageExtraction extractPage(const cv::Mat& image);

    /// 毫秒级顺手检测并解码图像中的全部二维码 (基于 OpenCV 内置 QRCodeDetector / quirc)。
    static std::vector<OcrQrResult> detectQrCodes(const cv::Mat& image);

    /// 提取文字并按行拼接为单个字符串。
    std::string recognizeToText(const cv::Mat& image);

    /// 从磁盘图片文件识别文字 (UTF-8 路径)。失败返回空串。
    std::string recognizeImageFile(const std::string& utf8Path);

    /// 判定图像是否属于深色模式 UI / 黑底浅字截图特征
    static bool isDarkBackgroundImage(const cv::Mat& image) noexcept;

    /// 针对黑底白字代码、暗色毛玻璃或低对比度截图执行 CLAHE 局部增强与智能反相预处理
    static cv::Mat enhanceImageForRecognition(const cv::Mat& image, bool* outInverted = nullptr);

    /// UTF-8 码点拆分与比例字符级子包围盒计算 (SSOT)
    static std::vector<std::string> splitUtf8Codepoints(std::string_view utf8);
    static size_t countUtf8Codepoints(std::string_view utf8) noexcept;
    static std::string sliceUtf8Codepoints(std::string_view utf8, size_t startIndex, size_t endExclusive);
    static std::vector<cv::Rect> buildProportionalCharBoxes(std::string_view utf8, const cv::Rect& bounds);

    /// 基于递归 XY-Cut (垂直空白投影分栏 + 水平跨栏切分) 将原始词块聚类为有序行结果，栏内大间距使用 \t 拼接，并同步生成字符级 charBoxes
    static std::vector<OcrResult> assembleLinesWithXYCut(
        std::vector<OcrWordToken> tokens,
        float scaleFactor = 1.0f,
        int borderPad = 0,
        int maxImageWidth = 0,
        int maxImageHeight = 0,
        const cv::Mat& sourceImage = {});

    /// 对已识别细粒度文本块（含精确 charBoxes）执行 UI 分栏感知切分与严苛水平基线对齐行聚类（双引擎共享 SSOT）
    static std::vector<OcrResult> assembleRecognizedBlocksWithLayout(
        std::vector<OcrResult> rawBlocks,
        const cv::Mat& sourceImage = {});

    /// 将含制表符的同行结果无损拆解为保留 layoutRegionId / alignedRowId 的紧凑原子块（供原图实景选字消除空白幽灵框）
    static std::vector<OcrResult> splitIntoAtomicBlocks(const std::vector<OcrResult>& lines);

    /// 对已识别文本块集合执行 XY-Cut 分栏阅读序重排与同行拼接（供贴图选词复制与全选复制复用）
    static std::string orderTextBlocksWithXYCut(
        const std::vector<OcrResult>& blocks,
        const char* lineEnding = "\r\n");

    /// 判定累计文本与下一行是否属于「未闭合 JSON 字符串软折行续行」，
    /// 若是则应无缝直连（禁止插入换行符破坏 RFC 8259 字符串语法，且防漏引号跨键误粘与跨分栏误粘）
    static bool shouldSeamlessJoinSoftWrappedJsonLine(
        std::string_view accumulatedText,
        std::string_view nextLineText,
        const cv::Rect* prevBox = nullptr,
        const cv::Rect* nextBox = nullptr) noexcept;

    /// 智能合并段落内因版面宽度产生的硬换行，保留真实段落/列表/代码块换行
    static std::wstring smartMergeParagraphs(const std::wstring& rawMultiLineText);
    static std::string smartMergeParagraphsUtf8(const std::string& rawMultiLineUtf8);

private:
    OcrEngine() = default;
    ~OcrEngine() = default;
    OcrEngine(const OcrEngine&) = delete;
    OcrEngine& operator=(const OcrEngine&) = delete;

    std::vector<OcrResult> extractWithWindowsOcr(const cv::Mat& image);

    bool m_available = false;
};

} // namespace tools3000::ocr

#endif // TOOLS3000_OCR_OCRENGINE_H
